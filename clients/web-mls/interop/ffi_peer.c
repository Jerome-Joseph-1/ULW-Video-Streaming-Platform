// A native MLS device over the FFI bridge (infra/e2ee/mls_ffi_bridge, ADR-0044), driven a line at
// a time on stdin so interop.mjs can put it in a chat room beside the WebAssembly client. It is
// the bridge's own code: the same calls the C++ harnesses make through infra::e2ee.
//
//   new <identity>          -> ok
//   kp                      -> ok <key package>
//   create <group id>       -> ok
//   join <welcome>          -> ok
//   add <key package>       -> ok <commit> <welcome>     (pending until merge or clear)
//   merge | clear           -> ok
//   encrypt <plaintext>     -> ok <message>
//   process <message>       -> ok <application|commit|proposal> <plaintext>
//   epoch | members         -> ok <n>
// Byte arguments and results are hex; identities and group ids are text. A failure answers
// `err <status>` with the bridge's status number.

#include "ulw/mls_ffi_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UlwMlsClient* client;
static UlwMlsGroup* group;

static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// Decodes `hex` in place; returns the byte count, or -1.
static long unhex(char* hex) {
    size_t n = strlen(hex);
    if (n % 2)
        return -1;
    uint8_t* out = (uint8_t*)hex;
    for (size_t i = 0; i < n / 2; ++i) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (long)(n / 2);
}

static void put_hex(const uint8_t* data, size_t len) {
    putchar(' ');
    for (size_t i = 0; i < len; ++i)
        printf("%02x", data[i]);
}

static void put_buffer(UlwMlsBuffer b) {
    put_hex(b.data, b.len);
    ulw_mls_buffer_free(b);
}

static int failed(UlwMlsStatus s) {
    if (s == ULW_MLS_STATUS_OK)
        return 0;
    printf("err %d\n", (int)s);
    return 1;
}

int main(void) {
    size_t cap = 0;
    char* line = NULL;
    ssize_t got;
    while ((got = getline(&line, &cap, stdin)) > 0) {
        if (line[got - 1] == '\n')
            line[got - 1] = '\0';
        char* arg = strchr(line, ' ');
        if (arg)
            *arg++ = '\0';
        else
            arg = line + strlen(line);
        UlwMlsBuffer a = {0}, b = {0};
        if (!strcmp(line, "new")) {
            if (failed(ulw_mls_client_new((const uint8_t*)arg, strlen(arg), &client)))
                continue;
            printf("ok");
        } else if (!strcmp(line, "kp")) {
            if (failed(ulw_mls_client_key_package(client, &a)))
                continue;
            printf("ok");
            put_buffer(a);
        } else if (!strcmp(line, "create")) {
            if (failed(ulw_mls_group_create(client, (const uint8_t*)arg, strlen(arg), &group)))
                continue;
            printf("ok");
        } else if (!strcmp(line, "join")) {
            long n = unhex(arg);
            if (n < 0) {
                puts("err hex");
                continue;
            }
            if (failed(ulw_mls_group_join(client, (const uint8_t*)arg, (size_t)n, &group)))
                continue;
            printf("ok");
        } else if (!strcmp(line, "add")) {
            long n = unhex(arg);
            if (n < 0) {
                puts("err hex");
                continue;
            }
            UlwMlsBytes kp = {(const uint8_t*)arg, (size_t)n};
            if (failed(ulw_mls_group_add(group, &kp, 1, &a, &b)))
                continue;
            printf("ok");
            put_buffer(a);
            put_buffer(b);
        } else if (!strcmp(line, "merge")) {
            if (failed(ulw_mls_group_merge_pending_commit(group)))
                continue;
            printf("ok");
        } else if (!strcmp(line, "clear")) {
            if (failed(ulw_mls_group_clear_pending_commit(group)))
                continue;
            printf("ok");
        } else if (!strcmp(line, "encrypt")) {
            long n = unhex(arg);
            if (n < 0) {
                puts("err hex");
                continue;
            }
            if (failed(ulw_mls_group_encrypt(group, (const uint8_t*)arg, (size_t)n, &a)))
                continue;
            printf("ok");
            put_buffer(a);
        } else if (!strcmp(line, "process")) {
            long n = unhex(arg);
            if (n < 0) {
                puts("err hex");
                continue;
            }
            UlwMlsReceived kind;
            if (failed(ulw_mls_group_process(group, (const uint8_t*)arg, (size_t)n, &kind, &a)))
                continue;
            printf("ok %s", kind == ULW_MLS_RECEIVED_APPLICATION ? "application"
                            : kind == ULW_MLS_RECEIVED_COMMIT    ? "commit"
                                                                 : "proposal");
            put_buffer(a);
        } else if (!strcmp(line, "epoch")) {
            uint64_t e;
            if (failed(ulw_mls_group_epoch(group, &e)))
                continue;
            printf("ok %llu", (unsigned long long)e);
        } else if (!strcmp(line, "members")) {
            size_t m;
            if (failed(ulw_mls_group_member_count(group, &m)))
                continue;
            printf("ok %zu", m);
        } else {
            printf("err command");
        }
        putchar('\n');
        fflush(stdout);
    }
    free(line);
    ulw_mls_group_free(group);
    ulw_mls_client_free(client);
    return 0;
}
