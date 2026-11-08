// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_frame.cpp
 * @brief Where a still of the camera comes from: a running stream's frame, else
 *        one snapshot fetched off-thread, else nothing; and that a snapshot
 *        landing after its owner is gone is dropped.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"
#include "camera_frame.h"
#include "camera_stream.h"

#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

CameraFrame solid(int w, int h) {
    CameraFrame f;
    f.w = w;
    f.h = h;
    f.bgr.assign(static_cast<size_t>(w) * h * 3, 0x7f);
    return f;
}

std::string benchy_jpeg() {
    std::ifstream in("assets/test_timelapse/benchy_timelapse_20260310.thumb.jpg", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct FakeCamera {
    CameraFrame stream;
    std::string url;
    std::function<CameraFrame(const std::string&, int, int)> decode = [](const std::string& j,
                                                                         int w, int h) {
        return CameraStream::decode_snapshot(j, w, h, {});
    };
    int fetches = 0;
    std::function<void(std::string)> pending;

    CameraFrameSources sources() {
        CameraFrameSources s;
        s.stream_frame = [this](int, int) { return stream; };
        s.snapshot = [this] {
            SnapshotTarget t;
            t.url = url;
            t.decode = decode;
            return t;
        };
        s.fetch = [this](const std::string&, std::function<void(std::string)> done) {
            ++fetches;
            pending = std::move(done);
        };
        return s;
    }
};

} // namespace

namespace {

/// A BGR image whose blue channel is its column and green channel its row.
std::vector<uint8_t> coordinate_image(int w, int h) {
    std::vector<uint8_t> image(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* pixel = image.data() + (static_cast<size_t>(y) * w + x) * 3;
            pixel[0] = static_cast<uint8_t>(x);
            pixel[1] = static_cast<uint8_t>(y);
            pixel[2] = 77;
        }
    }
    return image;
}

/// The source column and row of a pixel crop_cover_bgr wrote.
std::pair<int, int> source_of(const std::vector<uint8_t>& out, int stride, int x, int y) {
    const uint8_t* pixel =
        out.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
    return {pixel[0], pixel[1]};
}

} // namespace

TEST_CASE("crop_cover_bgr: the same proportions and size copy every pixel", "[camera_frame]") {
    const auto src = coordinate_image(4, 3);
    std::vector<uint8_t> out(4 * 3 * 3);

    REQUIRE(crop_cover_bgr(src.data(), 4, 3, 12, out.data(), 4, 3, 12));

    CHECK(out == src);
}

TEST_CASE("crop_cover_bgr: a wider source loses its left and right", "[camera_frame]") {
    // 8x4 into a square: the window is the middle 4x4, columns 2 to 5.
    const auto src = coordinate_image(8, 4);
    std::vector<uint8_t> out(4 * 4 * 3);

    REQUIRE(crop_cover_bgr(src.data(), 8, 4, 24, out.data(), 4, 4, 12));

    CHECK(source_of(out, 12, 0, 0) == std::pair<int, int>{2, 0});
    CHECK(source_of(out, 12, 3, 0) == std::pair<int, int>{5, 0});
    CHECK(source_of(out, 12, 0, 3) == std::pair<int, int>{2, 3});
    CHECK(source_of(out, 12, 3, 3) == std::pair<int, int>{5, 3});
}

TEST_CASE("crop_cover_bgr: a taller source loses its top and bottom", "[camera_frame]") {
    // 4x8 into a square: the window is the middle 4x4, rows 2 to 5.
    const auto src = coordinate_image(4, 8);
    std::vector<uint8_t> out(4 * 4 * 3);

    REQUIRE(crop_cover_bgr(src.data(), 4, 8, 12, out.data(), 4, 4, 12));

    CHECK(source_of(out, 12, 0, 0) == std::pair<int, int>{0, 2});
    CHECK(source_of(out, 12, 3, 3) == std::pair<int, int>{3, 5});
}

TEST_CASE("crop_cover_bgr: the window is scaled down to the box", "[camera_frame]") {
    const auto src = coordinate_image(8, 8);
    std::vector<uint8_t> out(2 * 2 * 3);

    REQUIRE(crop_cover_bgr(src.data(), 8, 8, 24, out.data(), 2, 2, 6));

    CHECK(source_of(out, 6, 0, 0) == std::pair<int, int>{0, 0});
    CHECK(source_of(out, 6, 1, 1) == std::pair<int, int>{4, 4});
}

TEST_CASE("crop_cover_bgr: a smaller source is enlarged to fill the box", "[camera_frame]") {
    const auto src = coordinate_image(2, 2);
    std::vector<uint8_t> out(4 * 4 * 3);

    REQUIRE(crop_cover_bgr(src.data(), 2, 2, 6, out.data(), 4, 4, 12));

    CHECK(source_of(out, 12, 3, 3) == std::pair<int, int>{1, 1});
}

TEST_CASE("crop_cover_bgr: strides wider than the pixels are honoured", "[camera_frame]") {
    constexpr int kSrcStride = 16;
    constexpr int kDstStride = 20;
    std::vector<uint8_t> src(kSrcStride * 2, 0);
    src[kSrcStride + 3] = 9; // Pixel (1, 1), blue
    std::vector<uint8_t> out(kDstStride * 2, 0xEE);

    REQUIRE(crop_cover_bgr(src.data(), 2, 2, kSrcStride, out.data(), 2, 2, kDstStride));

    CHECK(out[kDstStride + 3] == 9);
    CHECK(out[6] == 0xEE); // Past the row's pixels: left alone
}

TEST_CASE("crop_cover_bgr: degenerate input fails and writes nothing", "[camera_frame]") {
    const auto src = coordinate_image(2, 2);
    std::vector<uint8_t> out(12, 0xEE);
    const auto untouched = out;

    CHECK_FALSE(crop_cover_bgr(nullptr, 2, 2, 6, out.data(), 2, 2, 6));
    CHECK_FALSE(crop_cover_bgr(src.data(), 2, 2, 6, nullptr, 2, 2, 6));
    CHECK_FALSE(crop_cover_bgr(src.data(), 0, 2, 6, out.data(), 2, 2, 6));
    CHECK_FALSE(crop_cover_bgr(src.data(), 2, 0, 6, out.data(), 2, 2, 6));
    CHECK_FALSE(crop_cover_bgr(src.data(), 2, 2, 6, out.data(), 0, 2, 6));
    CHECK_FALSE(crop_cover_bgr(src.data(), 2, 2, 6, out.data(), 2, -1, 6));
    CHECK(out == untouched);
}

TEST_CASE("downscale_bgr fits the box, keeps aspect, never enlarges", "[camera_frame]") {
    auto big = solid(640, 360);
    auto f = downscale_bgr(big.bgr.data(), 640, 360, 640 * 3, 320, 320);
    CHECK(f.w == 320);
    CHECK(f.h == 180);
    CHECK(f.bgr.size() == 320u * 180u * 3u);

    auto small = solid(100, 50);
    auto g = downscale_bgr(small.bgr.data(), 100, 50, 100 * 3, 320, 320);
    CHECK(g.w == 100);
    CHECK(g.h == 50);
}

TEST_CASE_METHOD(LVGLTestFixture, "acquire_camera_frame picks stream, snapshot, or nothing",
                 "[camera_frame]") {
    AsyncLifetimeGuard owner;
    int late = 0;
    auto on_late = [&](CameraFrame f) {
        CHECK_FALSE(f.empty());
        ++late;
    };

    SECTION("a running stream wins; no fetch is started") {
        FakeCamera cam;
        cam.stream = solid(40, 30);
        cam.url = "http://cam/snapshot";
        auto f = acquire_camera_frame(cam.sources(), 320, 144, owner.token(), on_late);
        CHECK(f.w == 40);
        CHECK(cam.fetches == 0);
    }

    SECTION("no stream: one snapshot is fetched and lands on the main thread") {
        auto jpeg = benchy_jpeg();
        REQUIRE_FALSE(jpeg.empty());
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        auto f = acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        CHECK(f.empty());
        REQUIRE(cam.fetches == 1);
        cam.pending(jpeg);
        CHECK(late == 0); // not until the main thread drains
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 1);
    }

    SECTION("rotation 90 fits the box as displayed, not as decoded") {
        auto jpeg = benchy_jpeg(); // 175x182, near square: use a tall box to tell axes apart
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        cam.decode = [](const std::string& j, int w, int h) {
            return CameraStream::decode_snapshot(j, w, h, {CameraRotation::Rotate90, false, false});
        };
        int w = 0, h = 0;
        acquire_camera_frame(cam.sources(), 200, 40, owner.token(), [&](CameraFrame f) {
            w = f.w;
            h = f.h;
        });
        cam.pending(jpeg);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(w > 0);
        CHECK(w <= 200);
        CHECK(h <= 40);
        CHECK(w >= h); // the source is taller than wide; rotated it is not
    }

    SECTION("a failed fetch delivers nothing") {
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        cam.pending({});
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 0);
    }

    SECTION("a snapshot landing after its owner closed is dropped") {
        auto jpeg = benchy_jpeg();
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        owner.invalidate();
        cam.pending(jpeg);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 0);
    }

    SECTION("no camera: text only, nothing fetched") {
        FakeCamera cam;
        auto f = acquire_camera_frame(cam.sources(), 320, 144, owner.token(), on_late);
        CHECK(f.empty());
        CHECK(cam.fetches == 0);
    }
}

TEST_CASE("transform_from_config XORs the user's flip with Moonraker's", "[camera_frame]") {
    WebcamInfo feed;
    feed.flip_horizontal = true;
    nlohmann::json cfg = {{"rotation", 270}, {"flip_h", true}, {"flip_v", true}};
    auto t = CameraStream::transform_from_config(cfg, feed);
    CHECK(t.rotation == CameraRotation::Rotate270);
    CHECK_FALSE(t.flip_h); // Moonraker flips, user flips: net none
    CHECK(t.flip_v);
}
