// Feeds arbitrary bytes to the OpenMLS bridge wherever it decodes input: as a welcome to join
// by, as a message for a member to process, and as a key package to add. Every call must answer
// with a status; a crash, a sanitizer report or a leak is the finding. The group is fixed and
// shared by all inputs, and an input that happens to be accepted (a real key package from the
// corpus) has its commit cleared, so one input never changes what the next one meets.
//
// Input: byte 0 picks the entry point (modulo 3); the rest is the bytes it decodes.

#include "ulw/mls_ffi_bridge.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

namespace {

struct Fixture {
    UlwMlsClient* alice = nullptr;
    UlwMlsClient* bob = nullptr;
    UlwMlsGroup* alice_group = nullptr;
    UlwMlsGroup* bob_group = nullptr;
};

void require(bool ok) {
    if (!ok) {
        std::abort();
    }
}

const std::uint8_t* bytes_of(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char and uint8_t.
    return reinterpret_cast<const std::uint8_t*>(text.data());
}

// Alice's group with Bob in it, built once. Deliberately never freed: libFuzzer keeps the
// process alive across inputs, and the leak check looks for what one input leaves behind.
Fixture& fixture() {
    static Fixture f = [] {
        Fixture made;
        constexpr std::string_view kAlice = "alice";
        constexpr std::string_view kBob = "bob";
        constexpr std::string_view kRoom = "fuzz-room";
        require(ulw_mls_client_new(bytes_of(kAlice), kAlice.size(), &made.alice) ==
                ULW_MLS_STATUS_OK);
        require(ulw_mls_client_new(bytes_of(kBob), kBob.size(), &made.bob) == ULW_MLS_STATUS_OK);
        require(ulw_mls_group_create(made.alice, bytes_of(kRoom), kRoom.size(),
                                     &made.alice_group) == ULW_MLS_STATUS_OK);
        UlwMlsBuffer package{};
        require(ulw_mls_client_key_package(made.bob, &package) == ULW_MLS_STATUS_OK);
        const UlwMlsBytes entry{.data = package.data, .len = package.len};
        UlwMlsBuffer commit{};
        UlwMlsBuffer welcome{};
        require(ulw_mls_group_add(made.alice_group, &entry, 1, &commit, &welcome) ==
                ULW_MLS_STATUS_OK);
        require(ulw_mls_group_merge_pending_commit(made.alice_group) == ULW_MLS_STATUS_OK);
        require(ulw_mls_group_join(made.bob, welcome.data, welcome.len, &made.bob_group) ==
                ULW_MLS_STATUS_OK);
        ulw_mls_buffer_free(package);
        ulw_mls_buffer_free(commit);
        ulw_mls_buffer_free(welcome);
        return made;
    }();
    return f;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const Fixture& f = fixture();
    const std::span input{data, size};
    const std::uint8_t* body = input.subspan(1).data();
    const std::size_t len = size - 1;
    switch (input[0] % 3) {
    case 0: {
        UlwMlsGroup* joined = nullptr;
        const UlwMlsStatus s = ulw_mls_group_join(f.bob, body, len, &joined);
        require((s == ULW_MLS_STATUS_OK) == (joined != nullptr));
        ulw_mls_group_free(joined);
        break;
    }
    case 1: {
        UlwMlsReceived kind{};
        UlwMlsBuffer plaintext{};
        const UlwMlsStatus s = ulw_mls_group_process(f.bob_group, body, len, &kind, &plaintext);
        require(s == ULW_MLS_STATUS_OK || plaintext.data == nullptr);
        ulw_mls_buffer_free(plaintext);
        break;
    }
    default: {
        const UlwMlsBytes entry{.data = body, .len = len};
        UlwMlsBuffer commit{};
        UlwMlsBuffer welcome{};
        const UlwMlsStatus s = ulw_mls_group_add(f.alice_group, &entry, 1, &commit, &welcome);
        if (s == ULW_MLS_STATUS_OK) {
            require(ulw_mls_group_clear_pending_commit(f.alice_group) == ULW_MLS_STATUS_OK);
        } else {
            require(commit.data == nullptr && welcome.data == nullptr);
        }
        ulw_mls_buffer_free(commit);
        ulw_mls_buffer_free(welcome);
        break;
    }
    }
    return 0;
}
