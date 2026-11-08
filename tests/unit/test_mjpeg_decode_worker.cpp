// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mjpeg_decode_worker.h"
#include "netd_test_server.h" // helix_test::wait_until

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::MjpegDecodeWorker;

namespace {

constexpr int kUnlimited = -1;

/// A consumer that records every frame, and parks in the first until released,
/// the way a slow decode keeps the worker busy while newer frames arrive.
struct SlowConsumer {
    std::mutex mutex;
    std::condition_variable released_changed;
    bool released = false;
    std::vector<std::string> frames;
    std::atomic<int> count{0};

    void consume(std::string_view frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            frames.emplace_back(frame);
        }
        ++count;
        std::unique_lock<std::mutex> lock(mutex);
        released_changed.wait(lock, [this] { return released; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        released_changed.notify_all();
    }
};

} // namespace

TEST_CASE("MjpegDecodeWorker: a published frame reaches consume", "[camera]") {
    std::mutex mutex;
    std::string seen;
    std::atomic<int> count{0};
    MjpegDecodeWorker worker(
        [&](std::string_view frame) {
            std::lock_guard<std::mutex> lock(mutex);
            seen = frame;
            ++count;
        },
        [] { return kUnlimited; });

    worker.start();
    worker.publish("frame");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));
    worker.stop();

    CHECK(seen == "frame");
    CHECK(worker.frames_published() == 1);
}

TEST_CASE("MjpegDecodeWorker: frames published during a decode leave only the newest", "[camera]") {
    SlowConsumer consumer;
    MjpegDecodeWorker worker([&](std::string_view frame) { consumer.consume(frame); },
                             [] { return kUnlimited; });

    worker.start();
    worker.publish("first");
    REQUIRE(helix_test::wait_until([&] { return consumer.count.load() == 1; }, 2000));

    worker.publish("stale");
    worker.publish("newest");
    consumer.release();
    REQUIRE(helix_test::wait_until([&] { return consumer.count.load() == 2; }, 2000));
    worker.stop();

    CHECK(consumer.frames == std::vector<std::string>{"first", "newest"});
    CHECK(worker.frames_published() == 3);
}

TEST_CASE("MjpegDecodeWorker: a frame consumed once is not consumed again", "[camera]") {
    std::atomic<int> count{0};
    MjpegDecodeWorker worker([&](std::string_view) { ++count; }, [] { return kUnlimited; });

    worker.start();
    worker.publish("frame");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));
    worker.stop();

    CHECK(count.load() == 1);
}

TEST_CASE("MjpegDecodeWorker: paused discards frames", "[camera]") {
    std::atomic<int> count{0};
    std::atomic<int> asks{0};
    std::atomic<int> fps{0};
    MjpegDecodeWorker worker([&](std::string_view) { ++count; },
                             [&] {
                                 ++asks;
                                 return fps.load();
                             });

    worker.start();
    worker.publish("while paused");
    REQUIRE(helix_test::wait_until([&] { return asks.load() >= 1; }, 2000));
    CHECK(count.load() == 0);

    // Unpaused, the frame dropped above does not come back: only this one does.
    fps = kUnlimited;
    worker.publish("after");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));
    worker.stop();
    CHECK(count.load() == 1);
}

TEST_CASE("MjpegDecodeWorker: the cap paces frames and takes the newest when due", "[camera]") {
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::vector<std::string> frames;
    std::vector<Clock::time_point> consumed_at;
    std::atomic<int> count{0};
    std::atomic<int> asks{0};
    constexpr int kOneFramePerSecond = 1;
    MjpegDecodeWorker worker(
        [&](std::string_view frame) {
            std::lock_guard<std::mutex> lock(mutex);
            frames.emplace_back(frame);
            consumed_at.push_back(Clock::now());
            ++count;
        },
        [&] {
            ++asks;
            return kOneFramePerSecond;
        });

    worker.start();
    worker.publish("first");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));

    // Published one by one once the worker is in the pause, as a camera would.
    worker.publish("stale");
    REQUIRE(helix_test::wait_until([&] { return asks.load() == 2; }, 2000));
    worker.publish("newest");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 2; }, 3000));
    worker.stop();

    CHECK(frames == std::vector<std::string>{"first", "newest"});
    // A lower bound, so a slow machine cannot fail it. The 50 ms is the slack
    // between the worker's timestamp and the one taken here inside consume.
    CHECK(consumed_at[1] - consumed_at[0] >= std::chrono::milliseconds(950));
}

TEST_CASE("MjpegDecodeWorker: stop during the pause of a cap returns at once", "[camera]") {
    std::atomic<int> count{0};
    std::atomic<int> asks{0};
    constexpr int kOneFramePerSecond = 1;
    MjpegDecodeWorker worker([&](std::string_view) { ++count; },
                             [&] {
                                 ++asks;
                                 return kOneFramePerSecond;
                             });

    worker.start();
    worker.publish("first");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));
    worker.publish("never consumed");
    // The second ask is made holding the lock stop() needs, right before the
    // pause, so stop() cannot get in ahead of the pause.
    REQUIRE(helix_test::wait_until([&] { return asks.load() == 2; }, 2000));

    const auto began = std::chrono::steady_clock::now();
    worker.stop();

    CHECK(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500));
    CHECK(count.load() == 1);
}

TEST_CASE("MjpegDecodeWorker: start and stop are idempotent, and it starts again", "[camera]") {
    std::atomic<int> count{0};
    MjpegDecodeWorker worker([&](std::string_view) { ++count; }, [] { return kUnlimited; });

    worker.stop(); // before any start

    worker.start();
    worker.start();
    worker.publish("one");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 1; }, 2000));
    worker.stop();
    worker.stop();

    worker.start();
    worker.publish("two");
    REQUIRE(helix_test::wait_until([&] { return count.load() == 2; }, 2000));
    worker.stop();
}

TEST_CASE("MjpegDecodeWorker: stop with a frame waiting drops it", "[camera]") {
    SlowConsumer consumer;
    MjpegDecodeWorker worker([&](std::string_view frame) { consumer.consume(frame); },
                             [] { return kUnlimited; });

    worker.start();
    worker.publish("first");
    REQUIRE(helix_test::wait_until([&] { return consumer.count.load() == 1; }, 2000));
    worker.publish("waiting");
    consumer.release();
    worker.stop();

    // Either the worker took "waiting" before it saw the stop, or it never will.
    CHECK(consumer.count.load() <= 2);
}
