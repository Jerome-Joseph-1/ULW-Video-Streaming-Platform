/* Reports which io_uring features the reactor relies on are usable on this host.
 * Build: cc -O2 tools/io_uring_probe.c -o io_uring_probe -luring
 * Exit status is 0 only when everything the primary reactor needs is present. The ring and the
 * buffer rings have the reactor's own sizes (net/src/uring_reactor.cpp): a smaller ring can fit
 * under a limit the reactor's does not, so a probe that passed would say nothing. */
#include <sys/resource.h>

#include <liburing.h>
#include <stdio.h>
#include <string.h>

static int report(const char* what, int ok) {
    printf("%-34s %s\n", what, ok ? "yes" : "NO");
    return ok;
}

int main(void) {
    FILE* f = fopen("/proc/sys/kernel/io_uring_disabled", "r");
    int disabled = 0;
    if (f) {
        if (fscanf(f, "%d", &disabled) != 1)
            disabled = 0;
        fclose(f);
    }
    printf("%-34s %d\n", "kernel.io_uring_disabled", disabled);
    struct rlimit memlock;
    if (getrlimit(RLIMIT_MEMLOCK, &memlock) == 0) {
        if (memlock.rlim_cur == RLIM_INFINITY)
            printf("%-34s %s\n", "RLIMIT_MEMLOCK", "unlimited");
        else
            printf("%-34s %llu KiB\n", "RLIMIT_MEMLOCK",
                   (unsigned long long)memlock.rlim_cur / 1024);
    }

    struct io_uring ring;
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL;
    int rc = io_uring_queue_init_params(4096, &ring, &p);
    if (!report("setup SINGLE_ISSUER|DEFER_TASKRUN", rc == 0)) {
        printf("io_uring_queue_init_params: %s (errno %d)\n", strerror(-rc), -rc);
        return 1;
    }

    int ok = 1;
    struct io_uring_probe* probe = io_uring_get_probe_ring(&ring);
    if (!probe) {
        io_uring_queue_exit(&ring);
        return 1;
    }
    ok &=
        report("IORING_OP_ACCEPT (multishot)", io_uring_opcode_supported(probe, IORING_OP_ACCEPT));
    ok &= report("IORING_OP_RECV (multishot)", io_uring_opcode_supported(probe, IORING_OP_RECV));
    ok &=
        report("IORING_OP_ASYNC_CANCEL", io_uring_opcode_supported(probe, IORING_OP_ASYNC_CANCEL));
    report("IORING_OP_RECVMSG", io_uring_opcode_supported(probe, IORING_OP_RECVMSG));
    report("IORING_OP_SEND_ZC", io_uring_opcode_supported(probe, IORING_OP_SEND_ZC));
    report("IORING_OP_SENDMSG_ZC", io_uring_opcode_supported(probe, IORING_OP_SENDMSG_ZC));
    ok &= report("IORING_FEAT_NODROP", (p.features & IORING_FEAT_NODROP) != 0);
    io_uring_free_probe(probe);

    /* The stream receive ring (256 entries, one page) and the datagram one (512, two pages). */
    static const unsigned entries[] = {256, 512};
    for (int i = 0; i < 2; ++i) {
        int err = 0;
        struct io_uring_buf_ring* br = io_uring_setup_buf_ring(&ring, entries[i], i + 1, 0, &err);
        char what[40];
        snprintf(what, sizeof what, "provided buffer ring (%u)", entries[i]);
        ok &= report(what, br != NULL);
        if (!br)
            printf("io_uring_setup_buf_ring: %s (errno %d)\n", strerror(-err), -err);
        else
            io_uring_free_buf_ring(&ring, br, entries[i], i + 1);
    }

    rc = io_uring_register_files_sparse(&ring, 16);
    ok &= report("sparse direct descriptors", rc == 0);
    if (rc < 0)
        printf("io_uring_register_files_sparse: %s (errno %d)\n", strerror(-rc), -rc);

    io_uring_queue_exit(&ring);
    return ok && disabled == 0 ? 0 : 1;
}
