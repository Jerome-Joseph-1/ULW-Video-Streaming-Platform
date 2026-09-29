// syscall_probe NAME: makes one system call with all-zero arguments, and exits 0 if the process
// is still there afterwards. Run under the sandbox's syscall filter, a call it kills is the
// signal that ended the run; the arguments are of no interest, only whether the call was let
// through to fail.
#include <sys/syscall.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <unistd.h>

namespace {

struct Call {
    const char* name;
    long number;
};

constexpr std::array kCalls = {
    Call{.name = "getpid", .number = SYS_getpid},
    Call{.name = "ptrace", .number = SYS_ptrace},
    Call{.name = "mount", .number = SYS_mount},
    Call{.name = "keyctl", .number = SYS_keyctl},
    Call{.name = "bpf", .number = SYS_bpf},
    Call{.name = "io_uring_setup", .number = SYS_io_uring_setup},
    Call{.name = "socket", .number = SYS_socket},
    Call{.name = "unshare", .number = SYS_unshare},
    Call{.name = "setns", .number = SYS_setns},
    Call{.name = "kill", .number = SYS_kill},
    Call{.name = "process_vm_readv", .number = SYS_process_vm_readv},
    Call{.name = "chroot", .number = SYS_chroot},
};

} // namespace

int main(int argc, char** argv) {
    const std::span args(argv, static_cast<std::size_t>(argc));
    if (args.size() != 2) {
        return 2;
    }
    for (const Call& call : kCalls) {
        if (std::strcmp(call.name, args[1]) == 0) {
            static_cast<void>(::syscall(call.number, 0, 0, 0, 0, 0, 0));
            return 0;
        }
    }
    return 2;
}
