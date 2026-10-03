#include "config.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
        {"ULW_S3_ACCESS_KEY_ID", "AKIAEXAMPLE"},
        {"ULW_S3_SECRET_ACCESS_KEY", "example-secret"},
    };
};

TEST_F(WorkerConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->storage, StorageBackend::R2);
    EXPECT_EQ(config->storage_location, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(config->bucket, "ulw-media");
    EXPECT_EQ(config->node.view(), "transcode-worker-7d9f-x2x");
    EXPECT_EQ(config->scratch, "/var/cache/ulw-worker/transcode-worker-7d9f-x2x");
    EXPECT_TRUE(config->sandbox.empty());
    EXPECT_EQ(config->ffmpeg, "ffmpeg");
    EXPECT_EQ(config->ffprobe, "ffprobe");
    EXPECT_EQ(config->search_path, "/usr/local/bin:/usr/bin:/bin");
    EXPECT_EQ(config->ffmpeg_threads, 4U);
}

TEST_F(WorkerConfigTest, TheUserToDropToIsOptionalAndTakenAsGiven) {
    const auto unset = load();
    ASSERT_TRUE(unset);
    EXPECT_TRUE(unset->run_as_user.empty());
    env["ULW_RUN_AS_USER"] = "ulw";
    const auto set = load();
    ASSERT_TRUE(set);
    EXPECT_EQ(set->run_as_user, "ulw");
    EXPECT_FALSE(set->allow_root);
    env["ULW_ALLOW_ROOT"] = "1";
    EXPECT_TRUE(load()->allow_root);
    env["ULW_ALLOW_ROOT"] = "true";
    EXPECT_EQ(refused_variable(), "ULW_ALLOW_ROOT");
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

TEST_F(WorkerConfigTest, WhatCannotWorkIsRefusedBeforeAnythingStarts) {
    env["ULW_DATABASE_URL"] = "postgresql://ulw:Sup3r%Secret@db/ulw";
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().variable, "ULW_DATABASE_URL");
    EXPECT_EQ(config.error().reason.find("Sup3r"), std::string::npos);
    env["ULW_DATABASE_URL"] = "postgresql://ulw@db/ulw";
    env["ULW_R2_ACCOUNT_ID"] = "not an account";
    EXPECT_EQ(refused_variable(), "ULW_R2_ACCOUNT_ID");
    env["ULW_R2_ACCOUNT_ID"] = "0123456789abcdef0123456789abcdef";
    env.erase("ULW_S3_ACCESS_KEY_ID");
    EXPECT_EQ(refused_variable(), "ULW_S3_ACCESS_KEY_ID");
}

TEST_F(WorkerConfigTest, AnAccessKeyIdTheSignerWouldRefuseIsRefusedAtTheCheck) {
    for (const std::string& id : {std::string("AKIA EXAMPLE"), std::string(129, 'A')}) {
        env["ULW_S3_ACCESS_KEY_ID"] = id;
        EXPECT_EQ(refused_variable(), "ULW_S3_ACCESS_KEY_ID") << id;
    }
}

TEST_F(WorkerConfigTest, TheLogLevelIsOneOfFour) {
    EXPECT_EQ(load()->log_level, ops::Level::Info);
    env["ULW_LOG_LEVEL"] = "warn";
    EXPECT_EQ(load()->log_level, ops::Level::Warn);
    env["ULW_LOG_LEVEL"] = "loud";
    EXPECT_EQ(refused_variable(), "ULW_LOG_LEVEL");
}

TEST_F(WorkerConfigTest, TheWorkerTakesNoAuthSettingsAndKeepsItsPasswordOffTheCommandLine) {
    for (const ops::Setting& s : worker::settings()) {
        EXPECT_FALSE(s.env.starts_with("JWT") || s.env.starts_with("JWKS")) << s.env;
        EXPECT_EQ(s.secret, s.env == "ULW_DATABASE_URL" || s.env == "ULW_S3_ACCESS_KEY_ID" ||
                                s.env == "ULW_S3_SECRET_ACCESS_KEY")
            << s.env;
    }
    const std::vector<std::string_view> args{"--database-url=postgresql://u:pw@h/db"};
    EXPECT_FALSE(ops::parse_command_line(worker::settings(), args));
}

TEST_F(WorkerConfigTest, AFileSetsWhatTheEnvironmentLeavesUnset) {
    const auto entries = ops::toml::parse("[ffmpeg]\nthreads = 2\n[worker]\nnode_id = \"w-9\"\n");
    ASSERT_TRUE(entries);
    const ops::FileLayer file{.path = "w.toml", .entries = *entries, .private_to_owner = false};
    const auto layers = ops::Settings::layer(
        worker::settings(), &file,
        [this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        },
        ops::CommandLine{});
    ASSERT_TRUE(layers);
    const auto config = worker::load_config(layers->lookup());
    ASSERT_TRUE(config);
    EXPECT_EQ(config->ffmpeg_threads, 2U);
    EXPECT_EQ(config->node.view(), "w-9");
    // HOSTNAME still arrives through the layers, which is how the node id falls back to it.
    EXPECT_EQ(layers->get("HOSTNAME"), "transcode-worker-7d9f-x2x");
}

} // namespace
