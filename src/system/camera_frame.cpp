// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_frame.h"

#include "http_executor.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#if HELIX_HAS_CAMERA
#include "camera_stream.h"
#include "hv/requests.h"
#include "panel_widget_manager.h"
#include "webcam_selection.h"
#endif

namespace helix {

CameraFrame downscale_bgr(const uint8_t* src, int w, int h, int stride, int max_w, int max_h) {
    CameraFrame out;
    if (!src || w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0)
        return out;
    const double scale =
        std::min({1.0, static_cast<double>(max_w) / w, static_cast<double>(max_h) / h});
    out.w = std::max(1, static_cast<int>(w * scale));
    out.h = std::max(1, static_cast<int>(h * scale));
    out.bgr.resize(static_cast<size_t>(out.w) * out.h * 3);
    for (int y = 0; y < out.h; y++) {
        const uint8_t* row = src + static_cast<size_t>(y * h / out.h) * stride;
        uint8_t* dst = out.bgr.data() + static_cast<size_t>(y) * out.w * 3;
        for (int x = 0; x < out.w; x++)
            std::memcpy(dst + x * 3, row + static_cast<size_t>(x * w / out.w) * 3, 3);
    }
    return out;
}

bool crop_cover_bgr(const uint8_t* src, int w, int h, int stride, uint8_t* dst, int dst_w,
                    int dst_h, int dst_stride) {
    if (!src || !dst || w <= 0 || h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;
    // The largest window of the source with the box's proportions, centred.
    const int64_t source_wider = static_cast<int64_t>(w) * dst_h - static_cast<int64_t>(h) * dst_w;
    const int crop_w =
        source_wider > 0 ? std::max<int>(1, static_cast<int64_t>(h) * dst_w / dst_h) : w;
    const int crop_h =
        source_wider > 0 ? h : std::max<int>(1, static_cast<int64_t>(w) * dst_h / dst_w);
    const int left = (w - crop_w) / 2;
    const int top = (h - crop_h) / 2;

    std::vector<int> source_offset(static_cast<size_t>(dst_w));
    for (int x = 0; x < dst_w; x++)
        source_offset[static_cast<size_t>(x)] =
            (left + static_cast<int>(static_cast<int64_t>(x) * crop_w / dst_w)) * 3;

    for (int y = 0; y < dst_h; y++) {
        const uint8_t* row =
            src + static_cast<size_t>(top + static_cast<int64_t>(y) * crop_h / dst_h) * stride;
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < dst_w; x++) {
            const uint8_t* pixel = row + source_offset[static_cast<size_t>(x)];
            out[0] = pixel[0];
            out[1] = pixel[1];
            out[2] = pixel[2];
            out += 3;
        }
    }
    return true;
}

lv_draw_buf_t* to_draw_buf(const CameraFrame& f) {
    if (f.empty())
        return nullptr;
    lv_draw_buf_t* buf = lv_draw_buf_create(f.w, f.h, LV_COLOR_FORMAT_RGB888, 0);
    if (!buf)
        return nullptr;
    const size_t row = static_cast<size_t>(f.w) * 3;
    for (int y = 0; y < f.h; y++)
        std::memcpy(static_cast<uint8_t*>(buf->data) + static_cast<size_t>(y) * buf->header.stride,
                    f.bgr.data() + y * row, row);
    return buf;
}

CameraFrame acquire_camera_frame(const CameraFrameSources& src, int max_w, int max_h,
                                 LifetimeToken token,
                                 std::function<void(CameraFrame)> on_late_frame) {
    if (src.stream_frame) {
        CameraFrame f = src.stream_frame(max_w, max_h);
        if (!f.empty())
            return f;
    }
    SnapshotTarget target = src.snapshot ? src.snapshot() : SnapshotTarget{};
    if (target.url.empty() || !src.fetch || !target.decode)
        return {};
    src.fetch(target.url, [token, max_w, max_h, decode = std::move(target.decode),
                           cb = std::move(on_late_frame)](std::string body) mutable {
        if (body.empty())
            return;
        CameraFrame f = decode(body, max_w, max_h);
        if (f.empty())
            return;
        token.defer("CameraFrame::late_snapshot",
                    [cb = std::move(cb), f = std::move(f)]() mutable { cb(std::move(f)); });
    });
    return {};
}

#if HELIX_HAS_CAMERA

namespace {

/// Snapshots are a few hundred KB; anything near this is not a camera still.
constexpr size_t kMaxSnapshotBytes = 4 * 1024 * 1024;
constexpr int kSnapshotTimeoutSec = 5;

/// The camera the camera widget shows: its configured source (the auto-pick
/// when none is configured) and the widget's own config for rotation/flips.
struct ConfiguredFeed {
    std::optional<WebcamInfo> feed;
    nlohmann::json config;
};

ConfiguredFeed configured_feed() {
    ConfiguredFeed c;
    c.config = PanelWidgetManager::instance().get_widget_config("home").get_widget_config("camera");
    std::string source;
    if (c.config.is_object() && c.config.contains("source") && c.config["source"].is_string())
        source = c.config["source"].get<std::string>();
    c.feed = CameraStream::resolve_from_printer(source);
    return c;
}

} // namespace

CameraStream::Transform camera_view_transform(const WebcamInfo& cam) {
    auto c = configured_feed();
    return CameraStream::transform_from_config(webcam::view_config_for(cam.name, c.feed, c.config),
                                               cam);
}

CameraFrameSources live_camera_sources() {
    CameraFrameSources s;
    s.stream_frame = [](int max_w, int max_h) {
        auto c = configured_feed();
        return c.feed ? CameraStream::latest_running_frame(*c.feed, max_w, max_h) : CameraFrame{};
    };
    s.snapshot = [] {
        auto c = configured_feed();
        if (!c.feed)
            return SnapshotTarget{};
        SnapshotTarget t;
        t.url = c.feed->snapshot_url;
        t.decode = [tf = CameraStream::transform_from_config(c.config, *c.feed)](
                       const std::string& jpeg, int max_w, int max_h) {
            return CameraStream::decode_snapshot(jpeg, max_w, max_h, tf);
        };
        return t;
    };
    s.fetch = [](const std::string& url, std::function<void(std::string)> done) {
        helix::http::HttpExecutor::fast().submit([url, done = std::move(done)]() {
            auto req = std::make_shared<HttpRequest>();
            req->method = HTTP_GET;
            req->url = url;
            req->timeout = kSnapshotTimeoutSec;
            auto resp = requests::request(req);
            if (resp && resp->status_code >= 200 && resp->status_code < 300 &&
                resp->body.size() <= kMaxSnapshotBytes)
                done(std::move(resp->body));
            else
                done({});
        });
    };
    return s;
}

#else

CameraFrameSources live_camera_sources() {
    return {};
}

#endif

} // namespace helix
