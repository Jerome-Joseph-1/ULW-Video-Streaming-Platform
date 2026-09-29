#include "config.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>

namespace {

using live::Config;
using live::StorageBackend;

class LiveConfigTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<Config, live::ConfigError> load() const {
        return live::load_config([this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
    }

    [[nodiscard]] std::string refused_variable() const {
        const auto config = load();
        EXPECT_FALSE(config.has_value());
        return config ? std::string() : config.error().variable;
    }

    std::map<std::string, std::string, std::less<>> env{
        {"ULW_STREAM_ID", "show-1"},
        {"ULW_LIVE_INGEST_PORT", "1936"},
        {"ULW_LIVE_SRT_PASSPHRASE", "a passphrase of 24 chars"},
        {"ULW_R2_ACCOUNT_ID", "0123456789abcdef0123456789abcdef"},
        {"ULW_BUCKET", "ulw-media"},
    };
};

TEST_F(LiveConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->stream.str(), "show-1");
    EXPECT_EQ(config->ingest_host, "127.0.0.1");
    EXPECT_EQ(config->ingest_port, 1936);
    EXPECT_EQ(config->storage, StorageBackend::R2);
    EXPECT_EQ(config->bucket, "ulw-media");
    EXPECT_EQ(config->segment_seconds, 2U);
    EXPECT_EQ(config->window_segments, 10U);
    EXPECT_EQ(config->max_duration, core::Seconds{12 * 3600});
    EXPECT_EQ(config->srt_passphrase, "a passphrase of 24 chars");
    EXPECT_EQ(config->max_kbps, 20'000U);
    EXPECT_EQ(config->scratch, "/var/tmp/ulw-live/show-1");
    EXPECT_TRUE(config->sandbox.empty());
}

TEST_F(LiveConfigTest, ThePortMayBeZeroForAnEphemeralOne) {
    env["ULW_LIVE_INGEST_PORT"] = "0";
    ASSERT_TRUE(load());
    EXPECT_EQ(load()->ingest_port, 0);
}

TEST_F(LiveConfigTest, TheStreamAndItsPortAreRequired) {
    env.erase("ULW_STREAM_ID");
    EXPECT_EQ(refused_variable(), "ULW_STREAM_ID");
    env["ULW_STREAM_ID"] = "show";
    env.erase("ULW_LIVE_INGEST_PORT");
    EXPECT_EQ(refused_variable(), "ULW_LIVE_INGEST_PORT");
}

TEST_F(LiveConfigTest, ThePassphraseIsRequiredAndWithinWhatSrtTakes) {
    env.erase("ULW_LIVE_SRT_PASSPHRASE");
    EXPECT_EQ(refused_variable(), "ULW_LIVE_SRT_PASSPHRASE");
    for (const std::string& bad : {std::string("too short"), std::string(80, 'x')}) {
        env["ULW_LIVE_SRT_PASSPHRASE"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_LIVE_SRT_PASSPHRASE");
    }
    env["ULW_LIVE_SRT_PASSPHRASE"] = std::string(79, 'x');
    EXPECT_TRUE(load());
}

TEST_F(LiveConfigTest, ARefusedPassphraseIsNeverEchoedInTheReason) {
    env["ULW_LIVE_SRT_PASSPHRASE"] = "short-secret";
    env["ULW_LIVE_SRT_PASSPHRASE"] += std::string(80, 'y');
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().reason.find("short-secret"), std::string::npos);
}

TEST_F(LiveConfigTest, TheMaximumBitrateIsBounded) {
    for (const char* bad : {"499", "100001", "fast"}) {
        env["ULW_LIVE_MAX_KBPS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_LIVE_MAX_KBPS") << bad;
    }
    env["ULW_LIVE_MAX_KBPS"] = "8000";
    EXPECT_EQ(load()->max_kbps, 8000U);
}

TEST_F(LiveConfigTest, AStreamIdThatIsNotOneKeySegmentIsRefused) {
    env["ULW_STREAM_ID"] = "../other";
    EXPECT_EQ(refused_variable(), "ULW_STREAM_ID");
}

TEST_F(LiveConfigTest, APortOutOfRangeOrWithJunkIsRefused) {
    for (const char* bad : {"65536", "-1", "80x", " 80", "http"}) {
        env["ULW_LIVE_INGEST_PORT"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_LIVE_INGEST_PORT") << bad;
    }
}

TEST_F(LiveConfigTest, TheSegmentLengthStaysWithinWhatR2AndViewersAllow) {
    for (const char* ok : {"2", "10"}) {
        env["ULW_LIVE_SEGMENT_SECONDS"] = ok;
        EXPECT_TRUE(load()) << ok;
    }
    for (const char* bad : {"0", "1", "11", "2.5", ""}) {
        env["ULW_LIVE_SEGMENT_SECONDS"] = bad;
        if (std::string_view(bad).empty()) {
            EXPECT_TRUE(load());
            continue;
        }
        EXPECT_EQ(refused_variable(), "ULW_LIVE_SEGMENT_SECONDS") << bad;
    }
}

TEST_F(LiveConfigTest, TheWindowHoldsAtLeastThreeSegmentsAndAtMost64) {
    for (const char* bad : {"2", "65", "ten"}) {
        env["ULW_LIVE_WINDOW_SEGMENTS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_LIVE_WINDOW_SEGMENTS") << bad;
    }
    env["ULW_LIVE_WINDOW_SEGMENTS"] = "3";
    EXPECT_EQ(load()->window_segments, 3U);
    EXPECT_EQ(live::listed_segments(64), 128U);
}

TEST_F(LiveConfigTest, TheStreamCannotOutlastTheLongestVideo) {
    env["ULW_LIVE_MAX_HOURS"] = "13";
    EXPECT_EQ(refused_variable(), "ULW_LIVE_MAX_HOURS");
    env["ULW_LIVE_MAX_HOURS"] = "3";
    EXPECT_EQ(load()->max_duration, core::Seconds{3 * 3600});
}

TEST_F(LiveConfigTest, MinioNeedsAnEndpointAndABucket) {
    env["ULW_STORAGE"] = "minio";
    EXPECT_EQ(refused_variable(), "ULW_S3_ENDPOINT");
    env["ULW_S3_ENDPOINT"] = "http://127.0.0.1:9000";
    ASSERT_TRUE(load());
    EXPECT_EQ(load()->storage, StorageBackend::Minio);
    env.erase("ULW_BUCKET");
    EXPECT_EQ(refused_variable(), "ULW_BUCKET");
}

TEST_F(LiveConfigTest, TheFilesystemBackendNeedsARootAndNoBucket) {
    env["ULW_STORAGE"] = "fs";
    env.erase("ULW_BUCKET");
    EXPECT_EQ(refused_variable(), "ULW_FS_ROOT");
    env["ULW_FS_ROOT"] = "/srv/objects";
    ASSERT_TRUE(load());
    EXPECT_EQ(load()->storage, StorageBackend::Filesystem);
}

TEST_F(LiveConfigTest, AnUnknownStorageKindIsRefused) {
    env["ULW_STORAGE"] = "gcs";
    EXPECT_EQ(refused_variable(), "ULW_STORAGE");
}

TEST_F(LiveConfigTest, PathsMustBeAbsoluteAndTheScratchIsNamedForTheStream) {
    env["ULW_SCRATCH_DIR"] = "relative/dir";
    EXPECT_EQ(refused_variable(), "ULW_SCRATCH_DIR");
    env["ULW_SCRATCH_DIR"] = "/data";
    EXPECT_EQ(load()->scratch, "/data/show-1");
    env["ULW_SANDBOX_BIN"] = "bin/ulw_sandbox";
    EXPECT_EQ(refused_variable(), "ULW_SANDBOX_BIN");
}

TEST_F(LiveConfigTest, EmptyValuesCountAsUnset) {
    env["ULW_LIVE_INGEST_HOST"] = "";
    env["ULW_FFMPEG"] = "";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->ingest_host, "127.0.0.1");
    EXPECT_EQ(config->ffmpeg, "ffmpeg");
}

TEST_F(LiveConfigTest, RecordingIsOffUnlessADatabaseAndAnOwnerAreBothGiven) {
    EXPECT_FALSE(load()->recording.has_value());
    env["ULW_DATABASE_URL"] = "postgresql://ulw:secret@db/ulw";
    EXPECT_EQ(refused_variable(), "ULW_STREAM_OWNER");
    env["ULW_STREAM_OWNER"] = "auth0|streamer";
    const auto config = load();
    ASSERT_TRUE(config && config->recording);
    EXPECT_EQ(config->recording->database_url, "postgresql://ulw:secret@db/ulw");
    EXPECT_EQ(config->recording->owner.view(), "auth0|streamer");
    env.erase("ULW_DATABASE_URL");
    EXPECT_EQ(refused_variable(), "ULW_DATABASE_URL");
}

TEST_F(LiveConfigTest, AnOwnerThatIsNotAUserIdIsRefusedWithoutEchoingTheDatabaseUrl) {
    env["ULW_DATABASE_URL"] = "postgresql://ulw:Sup3rSecret@db/ulw";
    env["ULW_STREAM_OWNER"] = "no spaces allowed";
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().variable, "ULW_STREAM_OWNER");
    EXPECT_EQ(config.error().reason.find("Sup3r"), std::string::npos);
}

} // namespace
