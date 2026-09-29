#include "config.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>

namespace {

using worker::Config;
using worker::StorageBackend;

class WorkerConfigTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<Config, worker::ConfigError> load() const {
        return worker::load_config([this](std::string_view name) -> std::optional<std::string> {
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
        {"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
        {"ULW_R2_ACCOUNT_ID", "0123456789abcdef0123456789abcdef"},
        {"ULW_BUCKET", "ulw-media"},
        {"HOSTNAME", "transcode-worker-7d9f-x2x"},
    };
};

TEST_F(WorkerConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->storage, StorageBackend::R2);
    EXPECT_EQ(config->storage_location, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(config->bucket, "ulw-media");
    EXPECT_EQ(config->node.view(), "transcode-worker-7d9f-x2x");
    EXPECT_EQ(config->scratch, "/var/tmp/ulw-worker/transcode-worker-7d9f-x2x");
    EXPECT_TRUE(config->sandbox.empty());
    EXPECT_EQ(config->ffmpeg, "ffmpeg");
    EXPECT_EQ(config->ffprobe, "ffprobe");
    EXPECT_EQ(config->search_path, "/usr/local/bin:/usr/bin:/bin");
    EXPECT_EQ(config->ffmpeg_threads, 4U);
}

TEST_F(WorkerConfigTest, EachRequiredVariableIsNamedWhenMissing) {
    for (const std::string name : {"ULW_DATABASE_URL", "ULW_R2_ACCOUNT_ID", "ULW_BUCKET"}) {
        const std::string saved = env.at(name);
        env.erase(name);
        EXPECT_EQ(refused_variable(), name);
        env[name] = saved;
    }
}

TEST_F(WorkerConfigTest, AnEmptyVariableCountsAsUnset) {
    env["ULW_DATABASE_URL"] = "";
    EXPECT_EQ(refused_variable(), "ULW_DATABASE_URL");
}

TEST_F(WorkerConfigTest, TheFilesystemBackendNeedsARootAndNoBucket) {
    env["ULW_STORAGE"] = "fs";
    env.erase("ULW_BUCKET");
    EXPECT_EQ(refused_variable(), "ULW_FS_ROOT");
    env["ULW_FS_ROOT"] = "/srv/ulw";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::Filesystem);
    EXPECT_EQ(config->storage_location, "/srv/ulw");
}

TEST_F(WorkerConfigTest, MinioIsFoundByItsEndpoint) {
    env["ULW_STORAGE"] = "minio";
    EXPECT_EQ(refused_variable(), "ULW_S3_ENDPOINT");
    env["ULW_S3_ENDPOINT"] = "http://127.0.0.1:9000";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::Minio);
    EXPECT_EQ(config->storage_location, "http://127.0.0.1:9000");
}

TEST_F(WorkerConfigTest, AnUnknownBackendIsRefused) {
    env["ULW_STORAGE"] = "gcs";
    EXPECT_EQ(refused_variable(), "ULW_STORAGE");
}

TEST_F(WorkerConfigTest, TheNodeIdOverridesTheHostnameAndMustBeALabel) {
    env["ULW_NODE_ID"] = "worker-a";
    ASSERT_TRUE(load());
    EXPECT_EQ(load()->node.view(), "worker-a");
    env["ULW_NODE_ID"] = "Worker_A";
    EXPECT_EQ(refused_variable(), "ULW_NODE_ID");
    env.erase("ULW_NODE_ID");
    env["HOSTNAME"] = "-bad-";
    EXPECT_EQ(refused_variable(), "HOSTNAME");
    env.erase("HOSTNAME");
    EXPECT_EQ(refused_variable(), "ULW_NODE_ID");
}

TEST_F(WorkerConfigTest, PathsMustBeAbsolute) {
    env["ULW_SCRATCH_DIR"] = "scratch";
    EXPECT_EQ(refused_variable(), "ULW_SCRATCH_DIR");
    env["ULW_SCRATCH_DIR"] = "/data/scratch";
    env["ULW_SANDBOX_BIN"] = "bin/ulw_sandbox";
    EXPECT_EQ(refused_variable(), "ULW_SANDBOX_BIN");
    env["ULW_SANDBOX_BIN"] = "/opt/ulw/ulw_sandbox";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->scratch, "/data/scratch/transcode-worker-7d9f-x2x");
    EXPECT_EQ(config->sandbox, "/opt/ulw/ulw_sandbox");
}

TEST_F(WorkerConfigTest, ThreadsDefaultToAFixedNumberAndMayBeSet) {
    // Not the host's cores, which say nothing of the container's share of them.
    EXPECT_EQ(load()->ffmpeg_threads, 4U);
    env["ULW_FFMPEG_THREADS"] = "3";
    EXPECT_EQ(load()->ffmpeg_threads, 3U);
    for (const char* bad : {"0", "65", "two", "-1", "4 "}) {
        env["ULW_FFMPEG_THREADS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_FFMPEG_THREADS") << bad;
    }
}

TEST_F(WorkerConfigTest, WorkersSharingAScratchRootEachGetADirectoryOfTheirOwn) {
    // Startup clears the scratch directory; one worker must not clear another's live jobs.
    const auto first = load();
    env["HOSTNAME"] = "transcode-worker-7d9f-y3y";
    const auto second = load();
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->scratch, second->scratch);
    EXPECT_EQ(first->scratch.parent_path(), second->scratch.parent_path());
}

TEST_F(WorkerConfigTest, TheChildrensPathComesFromOurs) {
    env["PATH"] = "/opt/ffmpeg/bin:/usr/bin";
    EXPECT_EQ(load()->search_path, "/opt/ffmpeg/bin:/usr/bin");
}

} // namespace
