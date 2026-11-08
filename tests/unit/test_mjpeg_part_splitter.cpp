// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mjpeg_part_splitter.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::MjpegPartSplitter;

namespace {

const std::string kBoundary = "--frame";

std::string part(const std::string& body) {
    return kBoundary +
           "\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body + "\r\n";
}

std::vector<std::string> drain(MjpegPartSplitter& splitter) {
    std::vector<std::string> bodies;
    while (auto body = splitter.next_part()) {
        bodies.emplace_back(*body);
    }
    return bodies;
}

MjpegPartSplitter splitter_with_boundary() {
    MjpegPartSplitter splitter;
    splitter.set_boundary(kBoundary);
    return splitter;
}

} // namespace

TEST_CASE("MjpegPartSplitter: a part is complete once the next boundary arrives", "[camera]") {
    auto splitter = splitter_with_boundary();

    const std::string first = part("JPEG-ONE");
    splitter.append(first.data(), first.size());
    CHECK(drain(splitter).empty());

    const std::string second = part("JPEG-TWO");
    splitter.append(second.data(), second.size());
    CHECK(drain(splitter) == std::vector<std::string>{"JPEG-ONE"});
}

TEST_CASE("MjpegPartSplitter: several parts in one piece all come out", "[camera]") {
    auto splitter = splitter_with_boundary();
    const std::string data = part("A") + part("B") + part("C") + kBoundary;
    splitter.append(data.data(), data.size());
    CHECK(drain(splitter) == std::vector<std::string>{"A", "B", "C"});
}

TEST_CASE("MjpegPartSplitter: the size of the pieces does not change the parts", "[camera]") {
    const std::string data = part("JPEG-ONE") + part("JPEG-TWO") + kBoundary;
    for (size_t piece : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{64}}) {
        DYNAMIC_SECTION("pieces of " << piece) {
            auto splitter = splitter_with_boundary();
            std::vector<std::string> bodies;
            for (size_t at = 0; at < data.size(); at += piece) {
                splitter.append(data.data() + at, std::min(piece, data.size() - at));
                for (std::string& body : drain(splitter)) {
                    bodies.push_back(std::move(body));
                }
            }
            CHECK(bodies == std::vector<std::string>{"JPEG-ONE", "JPEG-TWO"});
        }
    }
}

TEST_CASE("MjpegPartSplitter: a frame's search cost does not depend on the piece size",
          "[camera]") {
    // A frame of the size a 1280x720 camera sends, in libhv's 1 KB pieces. Searching
    // from the front on every piece examines over a hundred times the frame.
    const std::string frame = part(std::string(228 * 1024, 'x')) + part("next") + kBoundary;
    auto splitter = splitter_with_boundary();

    for (size_t at = 0; at < frame.size(); at += 1024) {
        splitter.append(frame.data() + at, std::min<size_t>(1024, frame.size() - at));
        drain(splitter);
    }

    CHECK(splitter.bytes_searched() <= 2 * frame.size());
}

TEST_CASE("MjpegPartSplitter: an empty stream has no parts", "[camera]") {
    auto splitter = splitter_with_boundary();
    CHECK_FALSE(splitter.next_part().has_value());
    splitter.append("", 0);
    CHECK_FALSE(splitter.next_part().has_value());
}

TEST_CASE("MjpegPartSplitter: without a boundary nothing is a part", "[camera]") {
    MjpegPartSplitter splitter;
    const std::string data = part("A") + part("B");
    splitter.append(data.data(), data.size());
    CHECK_FALSE(splitter.next_part().has_value());
}

TEST_CASE("MjpegPartSplitter: bytes before the first boundary are dropped", "[camera]") {
    auto splitter = splitter_with_boundary();

    const std::string garbage(4096, 'g');
    splitter.append(garbage.data(), garbage.size());
    CHECK_FALSE(splitter.next_part().has_value());
    CHECK(splitter.buffered() < kBoundary.size());

    const std::string data = part("A") + kBoundary;
    splitter.append(data.data(), data.size());
    CHECK(drain(splitter) == std::vector<std::string>{"A"});
}

TEST_CASE("MjpegPartSplitter: a part cut off in its headers waits for the rest", "[camera]") {
    auto splitter = splitter_with_boundary();
    const std::string data = part("A") + kBoundary;

    const size_t in_headers = kBoundary.size() + 5;
    splitter.append(data.data(), in_headers);
    CHECK_FALSE(splitter.next_part().has_value());

    splitter.append(data.data() + in_headers, data.size() - in_headers);
    CHECK(drain(splitter) == std::vector<std::string>{"A"});
}

TEST_CASE("MjpegPartSplitter: a part with no body comes out empty", "[camera]") {
    auto splitter = splitter_with_boundary();
    const std::string data = part("") + part("B") + kBoundary;
    splitter.append(data.data(), data.size());
    CHECK(drain(splitter) == std::vector<std::string>{"", "B"});
}

TEST_CASE("MjpegPartSplitter: clear forgets a half-seen part", "[camera]") {
    auto splitter = splitter_with_boundary();
    const std::string data = part("stale");
    splitter.append(data.data(), data.size() - 2);
    CHECK_FALSE(splitter.next_part().has_value());

    splitter.clear();
    CHECK(splitter.buffered() == 0);

    const std::string fresh = part("fresh") + kBoundary;
    splitter.append(fresh.data(), fresh.size());
    CHECK(drain(splitter) == std::vector<std::string>{"fresh"});
}
