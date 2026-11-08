// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file mjpeg_part_splitter.h
 * @brief Cuts an MJPEG (multipart/x-mixed-replace) byte stream into its JPEGs.
 */
namespace helix {

/// Buffers stream bytes and hands out each part's body. The HTTP client delivers
/// a stream in pieces of about 1 KB, so a search resumes where the previous one
/// stopped: restarting from the front costs a frame's size per piece, which on a
/// single-core board is slower than the camera and leaves the picture seconds
/// behind.
///
/// Confined to one thread (the stream thread).
class MjpegPartSplitter {
  public:
    /// Sets the part delimiter, with its leading "--". Drops what is buffered.
    void set_boundary(std::string boundary);

    void append(const char* data, size_t size);

    /// The next complete part's body, without its trailing CRLF; empty when the
    /// part has none. nullopt until more bytes arrive. The view is valid until the
    /// next call on this object.
    std::optional<std::string_view> next_part();

    /// Drops what is buffered and keeps the boundary.
    void clear();

    /// Clears, and gives the buffer's memory back.
    void release_memory();

    size_t buffered() const {
        return buffer_.size();
    }

    /// Bytes examined by searches so far: stays proportional to the bytes
    /// appended, however small the pieces they came in.
    uint64_t bytes_searched() const {
        return bytes_searched_;
    }

  private:
    enum class Phase { FindBoundary, FindHeadersEnd, FindNextBoundary };

    size_t search(std::string_view needle);
    void remember_unsearched_tail(size_t needle_size, size_t lowest);

    std::vector<char> buffer_;
    std::string boundary_;
    Phase phase_ = Phase::FindBoundary;
    size_t search_from_ = 0;
    size_t body_start_ = 0;
    size_t handed_out_ = 0; ///< Bytes of the part last returned, dropped by the next call.
    uint64_t bytes_searched_ = 0;
};

} // namespace helix
