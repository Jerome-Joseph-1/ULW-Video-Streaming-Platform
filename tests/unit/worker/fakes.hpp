#pragma once

#include "core/ports/job_queue.hpp"
#include "core/ports/object_transfer.hpp"
#include "core/ports/transcoder.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

// Every write any fake saw, in order, across all of them.
class Journal {
public:
    void add(std::string event) {
        const std::scoped_lock lock(mutex_);
        events_.push_back(std::move(event));
        changed_.notify_all();
    }
    [[nodiscard]] std::vector<std::string> events() const {
        const std::scoped_lock lock(mutex_);
        return events_;
    }
    // Waits, at most `limit`, for an event that `pred` accepts.
    template <class Pred> bool wait_for(Pred pred, std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] {
            return std::ranges::any_of(events_, [&](const std::string& e) { return pred(e); });
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> events_;
};

class FakeQueue final : public core::ports::IJobQueue {
public:
    explicit FakeQueue(Journal& journal, std::string name)
        : journal_(journal), name_(std::move(name)) {}

    // What the next calls answer; nullopt means the database is unreachable.
    void answer_heartbeat(std::optional<bool> held) {
        const std::scoped_lock lock(mutex_);
        heartbeat_ = held;
    }
    void answer_writes(std::optional<bool> held) {
        const std::scoped_lock lock(mutex_);
        writes_ = held;
    }

    core::ports::JobQueueResult<std::optional<core::ports::ClaimedJob>>
    claim(const core::NodeId& /*worker*/) override {
        return std::nullopt;
    }
    core::ports::JobQueueResult<bool> heartbeat(const core::ports::JobLease& /*lease*/,
                                                const core::NodeId& /*worker*/) override {
        journal_.add(name_ + " heartbeat");
        return reply(heartbeat_);
    }
    core::ports::JobQueueResult<bool> report_progress(const core::ports::JobLease& /*lease*/,
                                                      std::uint8_t percent) override {
        journal_.add(name_ + " progress " + std::to_string(percent));
        return reply(writes_);
    }
    core::ports::JobQueueResult<bool>
    finish(const core::ports::JobLease& /*lease*/, core::Millis duration,
           std::span<const core::ports::Rendition> renditions) override {
        std::string event = name_ + " finish " + std::to_string(duration.count());
        for (const auto& r : renditions) {
            event += " " + std::to_string(r.height) + ":" + std::to_string(r.bitrate_bps) + ":" +
                     r.playlist.str();
        }
        journal_.add(std::move(event));
        return reply(writes_);
    }
    core::ports::JobQueueResult<bool> fail(const core::ports::JobLease& /*lease*/,
                                           std::string_view reason, bool retryable) override {
        journal_.add(name_ + " fail " + (retryable ? "retryable " : "permanent ") +
                     std::string(reason));
        return reply(writes_);
    }
    core::ports::JobQueueResult<std::size_t> reap_expired() override { return 0; }
    void wait_for_work(core::Millis /*max_wait*/) override {}

private:
    core::ports::JobQueueResult<bool> reply(const std::optional<bool>& answer) {
        const std::scoped_lock lock(mutex_);
        if (!answer) {
            return std::unexpected(core::ports::JobQueueError::Unavailable);
        }
        return *answer;
    }

    Journal& journal_;
    std::string name_;
    std::mutex mutex_;
    std::optional<bool> heartbeat_ = true;
    std::optional<bool> writes_ = true;
};

class FakeTransfer final : public core::ports::IObjectTransfer {
public:
    explicit FakeTransfer(Journal& journal) : journal_(journal) {}

    // Runs after each upload, with the key it wrote.
    std::function<void(const std::string&)> after_upload;

    void put(const std::string& key, std::string bytes) { objects_[key] = std::move(bytes); }
    [[nodiscard]] const std::map<std::string, std::string>& objects() const { return objects_; }

    std::expected<std::uint64_t, core::ports::StorageError>
    size(const core::StorageKey& key) override {
        const auto it = objects_.find(key.str());
        if (it == objects_.end()) {
            return std::unexpected(core::ports::StorageError::NotFound);
        }
        return it->second.size();
    }
    std::expected<std::uint64_t, core::ports::StorageError>
    download(const core::StorageKey& key, const std::filesystem::path& destination) override {
        const auto it = objects_.find(key.str());
        if (it == objects_.end()) {
            return std::unexpected(core::ports::StorageError::NotFound);
        }
        std::ofstream(destination, std::ios::binary) << it->second;
        return it->second.size();
    }
    std::expected<void, core::ports::StorageError> upload(const std::filesystem::path& source,
                                                          const core::StorageKey& key,
                                                          const core::ContentType& type) override {
        std::string bytes(std::filesystem::file_size(source), '\0');
        std::ifstream(source, std::ios::binary)
            .read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        objects_[key.str()] = std::move(bytes);
        journal_.add("upload " + key.str() + " " + std::string(type.view()));
        if (after_upload) {
            after_upload(key.str());
        }
        return {};
    }

private:
    Journal& journal_;
    std::map<std::string, std::string> objects_;
};

// Writes what ffmpeg would for the ladder it is given; each call to run() first takes the
// next scripted failure, if one is left.
class FakeTranscoder final : public core::ports::ITranscoder {
public:
    core::ports::MediaInfo media{.width = 1280,
                                 .height = 720,
                                 .frame_rate = {.num = 30000, .den = 1001},
                                 .duration = core::Millis{8000},
                                 .has_audio = true};
    std::deque<core::ports::TranscodeError> run_failures;
    std::optional<core::ports::TranscodeError> probe_failure;
    std::optional<core::ports::TranscodeError> verify_failure;
    // Runs inside run(), before it writes anything, with the stop token it was given.
    std::function<void(core::ports::ITranscodeProgress&, const std::stop_token&)> during_run;
    // Runs once run() has written its output, with the directory it wrote.
    std::function<void(const std::filesystem::path&)> after_run;
    int runs = 0;

    core::ports::TranscodeResult<core::ports::MediaInfo> probe(const std::filesystem::path& input,
                                                               std::stop_token /*stop*/) override {
        if (!std::filesystem::exists(input)) {
            return std::unexpected(
                core::ports::TranscodeError{.kind = core::ports::TranscodeFailure::Rejected,
                                            .exit_code = 1,
                                            .detail = "no input"});
        }
        if (probe_failure) {
            return std::unexpected(*probe_failure);
        }
        return media;
    }

    core::ports::TranscodeResult<core::ports::TranscodeStats>
    run(const std::filesystem::path& /*input*/, const std::filesystem::path& out_dir,
        const core::ports::MediaInfo& /*media*/, std::span<const core::Rung> ladder,
        core::ports::ITranscodeProgress& progress, std::stop_token stop) override {
        ++runs;
        if (during_run) {
            during_run(progress, stop);
        }
        if (stop.stop_requested()) {
            return std::unexpected(
                core::ports::TranscodeError{.kind = core::ports::TranscodeFailure::Stopped,
                                            .exit_code = 143,
                                            .detail = "stopped"});
        }
        if (!run_failures.empty()) {
            auto failure = run_failures.front();
            run_failures.pop_front();
            return std::unexpected(std::move(failure));
        }
        std::filesystem::create_directories(out_dir);
        std::ofstream(out_dir / "master.m3u8") << "#EXTM3U\n";
        for (std::size_t i = 0; i < ladder.size(); ++i) {
            const auto dir = out_dir / ladder[i].name;
            std::filesystem::create_directories(dir);
            std::ofstream(dir / ("init_" + std::to_string(i) + ".mp4")) << "init";
            std::ofstream(dir / "seg_00000.m4s") << "segment 0";
            std::ofstream(dir / "seg_00001.m4s") << "segment 1";
            std::ofstream(dir / "index.m3u8") << "#EXTM3U\n";
        }
        if (after_run) {
            after_run(out_dir);
        }
        return core::ports::TranscodeStats{.wall = core::Millis{4000}, .peak_rss_kib = 1234};
    }

    core::ports::TranscodeResult<void> verify(const std::filesystem::path& /*out_dir*/,
                                              const core::ports::MediaInfo& /*media*/,
                                              std::span<const core::Rung> /*ladder*/,
                                              std::stop_token /*stop*/) override {
        if (verify_failure) {
            return std::unexpected(*verify_failure);
        }
        return {};
    }
};

} // namespace ulw::test
