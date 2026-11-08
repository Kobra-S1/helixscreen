// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mjpeg_decode_worker.h"

#include "helix_thread.h"

#include <utility>

namespace helix {

MjpegDecodeWorker::MjpegDecodeWorker(std::function<void(std::string_view)> consume,
                                     std::function<int()> max_fps)
    : consume_(std::move(consume)), max_fps_(std::move(max_fps)) {}

MjpegDecodeWorker::~MjpegDecodeWorker() {
    stop();
}

void MjpegDecodeWorker::start() {
    if (!thread_.joinable()) {
        stopping_ = false;
        thread_ = helix::make_thread(&MjpegDecodeWorker::run, this);
    }
}

void MjpegDecodeWorker::publish(std::string_view frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        newest_.assign(frame.data(), frame.size());
        has_frame_ = true;
    }
    frames_published_++;
    changed_.notify_one();
}

void MjpegDecodeWorker::stop() {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    thread_.join();
    has_frame_ = false;
}

void MjpegDecodeWorker::run() {
    using Clock = std::chrono::steady_clock;

    std::string frame;
    Clock::time_point last_consumed{};
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return stopping_ || has_frame_; });
        if (stopping_) {
            return;
        }
        const int fps = max_fps_();
        if (fps == 0) {
            has_frame_ = false;
            continue;
        }
        if (fps > 0) {
            // Whatever is newest once the pause ends is the frame taken, not
            // the one that started the pause.
            const auto due = last_consumed + std::chrono::microseconds(1000000 / fps);
            if (changed_.wait_until(lock, due, [this] { return stopping_; })) {
                return;
            }
        }
        frame.swap(newest_);
        has_frame_ = false;
        lock.unlock();
        last_consumed = Clock::now();
        consume_(frame);
        lock.lock();
    }
}

} // namespace helix
