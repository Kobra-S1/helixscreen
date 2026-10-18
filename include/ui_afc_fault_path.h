// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <optional>
#include <string>
#include <string_view>

/**
 * @file ui_afc_fault_path.h
 * @brief Publishes an AFC fault's stop point to the `afc_fault_segment` subject.
 *
 * The graphic itself lives in `ui_xml/components/afc_fault_path.xml` — four labelled
 * stops (Spool, Lane, Hub, Toolhead), the three gaps between them, and a caption naming
 * the failing gap in words, all bound to this one int subject. C++ only decides *where*
 * the filament stopped; XML owns how it looks.
 *
 * Subject values are `PathSegment` (`include/ams_types.h`), with 0 (`NONE`) meaning
 * "no recognised position" — the component binds `hidden` to that, so an unrecognised
 * or non-AFC fault shows nothing at all and the modal renders exactly as it did before.
 */

namespace helix::ui {

/**
 * @brief Register the `afc_fault_segment` subject and the `afc_fault_path` component.
 *
 * Call once from `register_xml_components()`, before any XML that embeds
 * `<afc_fault_path/>` is registered. Idempotent. Self-registers its teardown with
 * `StaticSubjectRegistry` so the subject dies before `lv_deinit()`.
 */
void afc_fault_path_register();

/**
 * @brief Publish the stop point an AFC fault sentence in @p message names.
 *
 * Sets `afc_fault_segment` to that `PathSegment`, or to 0 (graphic hidden) when
 * @p message names none. A modal that embeds `<afc_fault_path/>` calls this with
 * its own text before it is built, so the strip always describes the text on
 * screen and never a fault some earlier modal published. Registers the subject
 * again when a printer switch has torn it down.
 *
 * Main thread only: it writes an LVGL subject, which fires observers.
 *
 * @return The recognised segment, or std::nullopt.
 */
std::optional<PathSegment> afc_fault_path_publish(std::string_view message);

/**
 * @brief Publish an AFC fault message's stop point and strip its ASCII art.
 *
 * Sets `afc_fault_segment` to the recognised `PathSegment`, or to 0 when the message
 * is not one of AFC's five diagram-bearing faults — which also hides the graphic, so
 * every caller must route its message through here, including the ones that turn out
 * to be unrecognised. Otherwise a previous fault's marker stays on screen.
 *
 * Main thread only: it writes an LVGL subject, which fires observers.
 *
 * @param message The fault message as it reached the modal.
 * @return The message with AFC's diagram rows removed, or unchanged when unrecognised.
 */
std::string afc_fault_path_apply(const std::string& message);

} // namespace helix::ui
