// Copyright 2023 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gematria/datasets/find_accessed_addrs.h"

#include <bits/types/siginfo_t.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "absl/random/random.h"
#include "absl/random/uniform_int_distribution.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gematria/datasets/block_wrapper.h"

namespace gematria {
namespace {

// This is the address which we put the code at by default. This particular
// address is chosen because it's in the middle of a large empty range, under
// normal conditions, and we'd like rip-relative addressing to be likely to hit
// unmapped memory. If this address isn't available, we'll accept whatever the
// kernel gives us. But if possible, we use this address.
constexpr uintptr_t kDefaultCodeLocation = 0x2b00'0000'0000;

// The data which is communicated from the child to the parent. The protocol is
// that the child will either write nothing (if it crashes unexpectedly before
// getting the chance to write to the pipe), or it will write one copy of this
// struct. If the inner StatusCode is not OK, the rest of the fields other than
// status_message are undefined. Alignment / size of data types etc. isn't an
// issue here since this is only ever used for IPC with a forked process, so the
// ABI will be identical.
struct PipedData {
  absl::StatusCode status_code;
  char status_message[1024];
  uintptr_t code_address;
};

PipedData MakePipedData() {
  PipedData piped_data;

  // Zero out the entire object, not just each field individually -- we'll be
  // writing the entire thing out to the pipe as a byte array, and if we just
  // initialize all the fields we'll leave any padding uninitialized, which will
  // make msan unhappy when we write it to the pipe.
  memset(&piped_data, 0, sizeof(piped_data));
  return piped_data;
}

bool IsRetryable(int err) {
  return err == EINTR || err == EAGAIN || err == EWOULDBLOCK;
}

absl::Status WriteAll(int fd, const PipedData& piped_data) {
  auto data_span = absl::MakeConstSpan(
      reinterpret_cast<const uint8_t*>(&piped_data), sizeof piped_data);

  size_t current_offset = 0;
  while (current_offset < data_span.size()) {
    size_t to_write = data_span.size() - current_offset;

    ssize_t bytes_written;
    int err;
    do {
      bytes_written = write(fd, data_span.data() + current_offset, to_write);
      err = errno;
    } while (IsRetryable(err));

    if (bytes_written < 0) {
      return absl::ErrnoToStatus(err, "Failed to write to pipe");
    }

    current_offset += bytes_written;
  }

  close(fd);
  return absl::OkStatus();
}

absl::StatusOr<PipedData> ReadAll(int fd) {
  PipedData piped_data;
  auto data_span = absl::MakeSpan(reinterpret_cast<uint8_t*>(&piped_data),
                                  sizeof piped_data);

  size_t current_offset = 0;
  while (current_offset < data_span.size()) {
    size_t to_read = data_span.size() - current_offset;

    ssize_t bytes_read;
    int err;
    do {
      bytes_read = read(fd, data_span.data() + current_offset, to_read);
      err = errno;
    } while (IsRetryable(err));

    if (bytes_read < 0) {
      return absl::ErrnoToStatus(err, "Failed to read from pipe");
    }

    if (bytes_read == 0) {
      break;
    }
    current_offset += bytes_read;
  }

  if (current_offset != data_span.size()) {
    return absl::InternalError(absl::StrFormat(
        "Read less than expected from pipe (expected %uB, got %uB)",
        data_span.size(), current_offset));
  }
  close(fd);
  return piped_data;
}

uintptr_t AlignDown(uintptr_t x, size_t align) { return x - (x % align); }

std::string DumpRegs(const struct user_regs_struct& regs) {
  return absl::StrFormat(
      "\trsp=%016x rbp=%016x, rip=%016x\n"
      "\trax=%016x rbx=%016x, rcx=%016x\n"
      "\trdx=%016x rsi=%016x, rdi=%016x\n"
      "\t r8=%016x  r9=%016x, r10=%016x\n"
      "\tr11=%016x r12=%016x, r13=%016x\n"
      "\tr14=%016x r15=%016x",
      regs.rsp, regs.rbp, regs.rip, regs.rax, regs.rbx, regs.rcx, regs.rdx,
      regs.rsi, regs.rdi, regs.r8, regs.r9, regs.r10, regs.r11, regs.r12,
      regs.r13, regs.r14, regs.r15);
}

absl::Status ParentProcessInner(int child_pid, AccessedAddrs& accessed_addrs) {
  int status;
  waitpid(child_pid, &status, 0);

  if (!WIFSTOPPED(status)) {
    return absl::InternalError(absl::StrFormat(
        "Child terminated with an unexpected status: %d", status));
  }

  // At this point the child is stopped, and we are attached.
  // TODO(orodley): Since we don't set any ptrace options here, do we actually
  // need this initial stop and continue, or could the child just PTRACE_TRACEME
  // and keep going without raising an initial SIGSTOP?
  ptrace(PTRACE_CONT, child_pid, nullptr, nullptr);

  waitpid(child_pid, &status, 0);
  if (!WIFSTOPPED(status)) {
    return absl::InternalError(absl::StrFormat(
        "Child terminated with an unexpected status: %d", status));
  }

  int signal = WSTOPSIG(status);
  if (signal == SIGSEGV) {
    // SIGSEGV means the block tried to access some unmapped memory, as
    // expected.
    siginfo_t siginfo;
    ptrace(PTRACE_GETSIGINFO, child_pid, 0, &siginfo);
    uintptr_t addr = AlignDown(reinterpret_cast<uintptr_t>(siginfo.si_addr),
                               accessed_addrs.block_size);

    if (std::find(accessed_addrs.accessed_blocks.begin(),
                  accessed_addrs.accessed_blocks.end(),
                  addr) == accessed_addrs.accessed_blocks.end()) {
      accessed_addrs.accessed_blocks.push_back(addr);
    }
    return absl::OkStatus();
  }
  if (signal == SIGABRT) {
    // SIGABRT means the block finished, and executed our after-block code which
    // terminates the process. So, it didn't access any memory.
    return absl::OkStatus();
  }

  if (signal == SIGFPE) {
    // Floating point exceptions are potentially fixable by setting different
    // register values, so return 'Invalid argument', which communicates this.
    return absl::InvalidArgumentError("Floating point exception");
  }

  // Any other case is an unexpected signal, so let's capture the registers for
  // ease of debugging.
  struct user_regs_struct registers;
  ptrace(PTRACE_GETREGS, child_pid, 0, &registers);

  if (signal == SIGBUS) {
    siginfo_t siginfo;
    ptrace(PTRACE_GETSIGINFO, child_pid, 0, &siginfo);
    return absl::InternalError(absl::StrFormat(
        "Child stopped with unexpected signal: %s, address %ul\n%s",
        strsignal(signal), (uint64_t)siginfo.si_addr, DumpRegs(registers)));
  }
  return absl::InternalError(
      absl::StrFormat("Child stopped with unexpected signal: %s\n%s",
                      strsignal(signal), DumpRegs(registers)));
}

absl::Status ParentProcess(int child_pid, int pipe_read_fd,
                           AccessedAddrs& accessed_addrs) {
  auto result = ParentProcessInner(child_pid, accessed_addrs);

  // Regardless of what happened, kill the child with SIGKILL. If we just detach
  // with PTRACE_DETACH and let the process resume, it will exit with whatever
  // signal it was about to exit with before we caught it. If that signal is
  // SIGSEGV then it could get caught by (e.g.) the terminal and printed. We
  // don't want that as SIGSEGV is actually normal and expected here, and this
  // would just be useless noise.
  int err = kill(child_pid, SIGKILL);
  if (err != 0) {
    char* err_str = strerror(err);
    return absl::InternalError(
        absl::StrFormat("Failed to kill child process: %s", err_str));
  }
  // We must wait on the child after killing it, otherwise it remains as a
  // zombie process.
  waitpid(child_pid, nullptr, 0);

  if (!result.ok()) {
    return result;
  }

  auto pipe_data = ReadAll(pipe_read_fd);
  if (!pipe_data.ok()) {
    return pipe_data.status();
  }

  if (pipe_data->status_code != absl::StatusCode::kOk) {
    return absl::Status(pipe_data->status_code, pipe_data->status_message);
  }

  accessed_addrs.code_location = pipe_data.value().code_address;

  return absl::OkStatus();
}

// This is used over memcpy as memcpy may get unmapped. Doing the copy manually
// with a for loop doesn't help, as the compiler will often replace such loops
// with a call to memcpy.
void repmovsb(void* dst, const void* src, size_t count) {
  asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}

[[noreturn]] void AbortChildProcess(int pipe_write_fd, absl::Status status) {
  auto piped_data = MakePipedData();
  piped_data.status_code = status.code();

  // Write as much of the message as we can fit into the piped data struct. We
  // subtract one from the size to ensure we always leave a null-terminator on
  // the end.
  size_t message_length =
      std::min(status.message().length(), sizeof piped_data.status_message - 1);
  repmovsb(piped_data.status_message, status.message().data(), message_length);

  WriteAll(pipe_write_fd, piped_data).IgnoreError();
  abort();
}

[[noreturn]] void ChildProcess(absl::Span<const uint8_t> basic_block,
                               int pipe_write_fd,
                               const AccessedAddrs& accessed_addrs) {
  // Make sure the parent is attached before doing anything that they might want
  // to listen for.
  ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
  raise(SIGSTOP);

  // This value will turn up when reading from newly-mapped blocks (see below).
  // Unmap it so that we can correctly segfault and detect we've accessed it.
  // If it fails, oh well. Not worth aborting for as we might not even access
  // this address.
  munmap(reinterpret_cast<void*>(0x800000000), 0x10000);

  // Map all the locations we've previously discovered this code accesses.
  for (uintptr_t accessed_location : accessed_addrs.accessed_blocks) {
    auto location_ptr = reinterpret_cast<void*>(accessed_location);
    void* mapped_address =
        mmap(location_ptr, accessed_addrs.block_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mapped_address == MAP_FAILED) {
      AbortChildProcess(pipe_write_fd, absl::InternalError(absl::StrFormat(
                                           "mapping previously discovered "
                                           "address %p failed",
                                           location_ptr)));
    }
    if (mapped_address != location_ptr) {
      // Use InvalidArgument only for the case where we couldn't map an address.
      // This can happen when an address is computed based on registers and ends
      // up not being valid to map, which is potentially fixable by running
      // again with different register values. By using a unique error code we
      // can distinguish this case easily.
      AbortChildProcess(
          pipe_write_fd,
          absl::InvalidArgumentError(absl::StrFormat(
              "tried to map previously discovered address %p, but mmap "
              "couldn't map this address\n",
              (void*)location_ptr)));
    }

    // Initialise every fourth byte to 8, leaving the rest as zeroes. This
    // ensures that every aligned 8-byte chunk will contain 0x800000008, which
    // is a mappable address, and every 4-byte chunk will contain 0x8, which is
    // a non-zero value which won't give SIGFPE if used with div.
    uint8_t* block = reinterpret_cast<uint8_t*>(mapped_address);
    for (int i = 0; i < accessed_addrs.block_size; i += 4) {
      block[i] = 8;
    }
  }

  // We copy in our before-block code which sets up registers, followed by the
  // code we're given, followed by our after-block code which cleanly exits the
  // process. Otherwise if it finishes without segfaulting it will just run over
  // into whatever is afterwards.
  const auto before_block = GetGematriaBeforeBlockCode();
  const auto after_block = GetGematriaAfterBlockCode();
  const auto total_block_size =
      before_block.size() + basic_block.size() + after_block.size();

  uintptr_t desired_code_location = accessed_addrs.code_location;
  if (desired_code_location == 0) {
    desired_code_location = kDefaultCodeLocation;
  }

  void* mapped_address =
      mmap(reinterpret_cast<void*>(desired_code_location), total_block_size,
           PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapped_address == MAP_FAILED) {
    perror("mmap failed");
    abort();
  }

  auto piped_data = MakePipedData();
  piped_data.status_code = absl::OkStatus().code();
  piped_data.code_address = reinterpret_cast<uintptr_t>(mapped_address);
  auto status = WriteAll(pipe_write_fd, piped_data);
  if (!status.ok()) {
    abort();
  }

  absl::Span<uint8_t> mapped_span = absl::MakeSpan(
      reinterpret_cast<uint8_t*>(mapped_address), total_block_size);
  std::copy(before_block.begin(), before_block.end(), &mapped_span[0]);
  std::copy(basic_block.begin(), basic_block.end(),
            &mapped_span[before_block.size()]);
  std::copy(after_block.begin(), after_block.end(),
            &mapped_span[before_block.size() + basic_block.size()]);

  auto mapped_func =
      reinterpret_cast<void (*)(const X64Regs* initial_regs)>(mapped_address);
  mapped_func(&accessed_addrs.initial_regs);

  // mapped_func should never return, but we can't put [[noreturn]] on a
  // function pointer. So stick this here to satisfy the compiler.
  abort();
}

absl::Status ForkAndTestAddresses(absl::Span<const uint8_t> basic_block,
                                  AccessedAddrs& accessed_addrs) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    int err = errno;
    return absl::ErrnoToStatus(
        err, "Failed to open pipe for communication with child process: %s");
  }
  int pipe_read_fd = pipe_fds[0];
  int pipe_write_fd = pipe_fds[1];

  pid_t pid = fork();
  switch (pid) {
    case -1: {
      int err = errno;
      return absl::ErrnoToStatus(err, "Failed to fork");
    }
    case 0:  // child
      // Child only writes to the pipe.
      close(pipe_read_fd);

      // ChildProcess doesn't return.
      ChildProcess(basic_block, pipe_write_fd, accessed_addrs);
    default:  // parent
      // Parent only reads from the pipe.
      close(pipe_write_fd);

      return ParentProcess(pid, pipe_read_fd, accessed_addrs);
  }
}

void RandomiseRegs(absl::BitGen& gen, X64Regs& regs) {
  // Pick between three values: 0, a low address, and a high address. These are
  // picked to try to maximise the chance that some combination will produce a
  // valid address when run through a wide range of functions. This is just a
  // first stab, there are likely better sets of values we could use here.
  constexpr int64_t kValues[] = {0, 0x15000, 0x1000000};
  absl::uniform_int_distribution<int> dist(0, std::size(kValues) - 1);
  auto random_reg = [&gen, &dist]() { return kValues[dist(gen)]; };

  regs.rax = random_reg();
  regs.rbx = random_reg();
  regs.rcx = random_reg();
  regs.rdx = random_reg();
  regs.rsi = random_reg();
  regs.rdi = random_reg();
  regs.rsp = random_reg();
  regs.rbp = random_reg();
  regs.r8 = random_reg();
  regs.r9 = random_reg();
  regs.r10 = random_reg();
  regs.r11 = random_reg();
  regs.r12 = random_reg();
  regs.r13 = random_reg();
  regs.r14 = random_reg();
  regs.r15 = random_reg();
}

}  // namespace

// TODO(orodley):
// * Set up registers to minimise chance of needing to map an unmappable or
//   already mapped address, the communicate the necessary set of register in
//   order for the returned addresses to be accessed.
// * Be more robust against the code trying to access addresses that happen to
//   already be mapped upon forking the process, and therefore not segfaulting,
//   so we can't observe the access.
// * Better error handling, return specific errors for different situations that
//   may occur, and document them well (e.g. handle SIGILL and return an error
//   stating that the code passed in is invalid, with a bad instruction at a
//   particular offset).
// * Much more complete testing.
absl::StatusOr<AccessedAddrs> FindAccessedAddrs(
    absl::Span<const uint8_t> basic_block) {
  // This value is chosen to be almost the lowest address that's able to be
  // mapped. We want it to be low so that even if a register is multiplied or
  // added to another register, it will still be likely to be within an
  // accessible region of memory. But it's very common to take small negative
  // offsets from a register as a memory address, so we want to leave some space
  // below so that such addresses will still be accessible.
  constexpr int64_t kInitialRegValue = 0x15000;

  absl::BitGen gen;

  AccessedAddrs accessed_addrs = {
      .code_location = 0,
      .block_size = static_cast<size_t>(getpagesize()),
      .accessed_blocks = {},
      .initial_regs =
          {
              .rax = kInitialRegValue,
              .rbx = kInitialRegValue,
              .rcx = kInitialRegValue,
              .rdx = kInitialRegValue,
              .rsi = kInitialRegValue,
              .rdi = kInitialRegValue,
              .rsp = kInitialRegValue,
              .rbp = kInitialRegValue,
              .r8 = kInitialRegValue,
              .r9 = kInitialRegValue,
              .r10 = kInitialRegValue,
              .r11 = kInitialRegValue,
              .r12 = kInitialRegValue,
              .r13 = kInitialRegValue,
              .r14 = kInitialRegValue,
              .r15 = kInitialRegValue,
          },
  };

  int n = 0;
  size_t num_accessed_blocks;
  do {
    num_accessed_blocks = accessed_addrs.accessed_blocks.size();
    auto status = ForkAndTestAddresses(basic_block, accessed_addrs);
    if (absl::IsInvalidArgument(status)) {
      if (n > 100) {
        return status;
      }

      accessed_addrs.accessed_blocks.clear();
      RandomiseRegs(gen, accessed_addrs.initial_regs);
    } else if (!status.ok()) {
      return status;
    }

    n++;
  } while (accessed_addrs.accessed_blocks.size() != num_accessed_blocks);

  return accessed_addrs;
}

}  // namespace gematria
