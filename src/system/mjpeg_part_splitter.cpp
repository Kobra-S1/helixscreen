// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mjpeg_part_splitter.h"

#include <algorithm>
#include <utility>

namespace helix {

namespace {
constexpr std::string_view kHeadersEnd = "\r\n\r\n";
}

void MjpegPartSplitter::set_boundary(std::string boundary) {
    boundary_ = std::move(boundary);
    clear();
}

void MjpegPartSplitter::append(const char* data, size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
}

void MjpegPartSplitter::clear() {
    buffer_.clear();
    phase_ = Phase::FindBoundary;
    search_from_ = 0;
    body_start_ = 0;
    handed_out_ = 0;
}

void MjpegPartSplitter::release_memory() {
    clear();
    buffer_.shrink_to_fit();
}

size_t MjpegPartSplitter::search(std::string_view needle) {
    const std::string_view view(buffer_.data(), buffer_.size());
    const size_t found = view.find(needle, search_from_);
    if (search_from_ < view.size()) {
        bytes_searched_ += (found == std::string_view::npos ? view.size() : found) - search_from_;
    }
    return found;
}

// A needle can straddle two pieces, so the next search starts early enough to
// see one whose start is already here.
void MjpegPartSplitter::remember_unsearched_tail(size_t needle_size, size_t lowest) {
    const size_t overlap = needle_size - 1;
    search_from_ = std::max(lowest, buffer_.size() > overlap ? buffer_.size() - overlap : 0);
}

std::optional<std::string_view> MjpegPartSplitter::next_part() {
    if (boundary_.empty()) {
        return std::nullopt;
    }
    if (handed_out_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(handed_out_));
        handed_out_ = 0;
    }

    if (phase_ == Phase::FindBoundary) {
        const size_t found = search(boundary_);
        if (found == std::string_view::npos) {
            // Nothing before a boundary belongs to a part: keep only what could
            // be the start of one.
            remember_unsearched_tail(boundary_.size(), 0);
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(search_from_));
            search_from_ = 0;
            return std::nullopt;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(found));
        search_from_ = boundary_.size();
        phase_ = Phase::FindHeadersEnd;
    }

    if (phase_ == Phase::FindHeadersEnd) {
        const size_t found = search(kHeadersEnd);
        if (found == std::string_view::npos) {
            remember_unsearched_tail(kHeadersEnd.size(), boundary_.size());
            return std::nullopt;
        }
        body_start_ = found + kHeadersEnd.size();
        search_from_ = body_start_;
        phase_ = Phase::FindNextBoundary;
    }

    const size_t next_boundary = search(boundary_);
    if (next_boundary == std::string_view::npos) {
        remember_unsearched_tail(boundary_.size(), body_start_);
        return std::nullopt;
    }

    size_t body_end = next_boundary;
    while (body_end > body_start_ &&
           (buffer_[body_end - 1] == '\r' || buffer_[body_end - 1] == '\n')) {
        body_end--;
    }
    // The next boundary starts the next part: it stays, found again at offset 0.
    handed_out_ = next_boundary;
    phase_ = Phase::FindBoundary;
    search_from_ = 0;
    return std::string_view(buffer_.data() + body_start_, body_end - body_start_);
}

} // namespace helix
