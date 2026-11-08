// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

/**
 * @file mjpeg_decode_worker.h
 * @brief Decodes the newest frame of an MJPEG stream on a thread of its own.
 */
namespace helix {

/// Holds one frame: publishing replaces whatever has not been consumed yet. A
/// reader that also decoded could only reach a newer frame by getting through
/// the older ones, so a board that falls behind stays behind by whatever the
/// sender and the network have queued. Reading apart from decoding lets the
/// queue drain whenever decoding is slow, and the frame shown is the newest.
///
/// consume runs on the worker's thread and must not throw. max_fps is asked
/// before each frame: above 0 paces consume to that rate, 0 discards frames,
/// below 0 is unlimited. start(), publish() and stop() are confined to one thread.
class MjpegDecodeWorker {
  public:
    MjpegDecodeWorker(std::function<void(std::string_view)> consume, std::function<int()> max_fps);
    ~MjpegDecodeWorker();

    MjpegDecodeWorker(const MjpegDecodeWorker&) = delete;
    MjpegDecodeWorker& operator=(const MjpegDecodeWorker&) = delete;

    /// Starts the thread. Throws std::system_error when it cannot be created.
    void start();

    void publish(std::string_view frame);

    /// Joins the thread, after any consume in progress returns. An unconsumed
    /// frame is dropped. A no-op when not started.
    void stop();

    uint64_t frames_published() const {
        return frames_published_;
    }

  private:
    friend class MjpegDecodeWorkerTestAccess;

    void run();

    std::function<void(std::string_view)> consume_;
    std::function<int()> max_fps_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string newest_;
    bool has_frame_ = false;
    bool stopping_ = false;
    uint64_t frames_published_ = 0; ///< Touched only by the publishing thread.
};

} // namespace helix
