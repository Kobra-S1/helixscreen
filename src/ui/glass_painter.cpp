// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glass_painter.h"

#include "lvgl/src/core/lv_obj_private.h"       // obj->styles: LVGL has no style iterator
#include "lvgl/src/core/lv_obj_style_private.h" // lv_obj_style_t fields
#include "theme_manager.h"

#include <cstdint>
#include <optional>

namespace helix::ui {

namespace {

/// One empty style per family, told apart by address. Adding one to a widget is the marker.
struct Markers {
    lv_style_t none;
    lv_style_t raised;
    lv_style_t surface;
    lv_style_t recessed;

    Markers() {
        lv_style_init(&none);
        lv_style_init(&raised);
        lv_style_init(&surface);
        lv_style_init(&recessed);
    }

    lv_style_t* of(GlassFamily f) {
        switch (f) {
        case GlassFamily::Raised:
            return &raised;
        case GlassFamily::Surface:
            return &surface;
        case GlassFamily::Recessed:
            return &recessed;
        case GlassFamily::None:
            break;
        }
        return &none;
    }
};

Markers& markers() {
    static Markers m;
    return m;
}

/// Whether `style` is one of `obj`'s enabled styles on `part`, in any state.
bool obj_has_style(const lv_obj_t* obj, const lv_style_t* style, lv_part_t part) {
    for (uint32_t i = 0; i < obj->style_cnt; i++) {
        const lv_obj_style_t& entry = obj->styles[i];
        if (entry.style == style && !entry.is_disabled &&
            lv_obj_style_get_selector_part(entry.selector) == part) {
            return true;
        }
    }
    return false;
}

bool has_role(const lv_obj_t* obj, StyleRole role, lv_part_t part) {
    return obj_has_style(obj, ThemeManager::instance().get_style(role), part);
}

std::optional<GlassFamily> marked_family(const lv_obj_t* obj) {
    Markers& m = markers();
    for (GlassFamily f :
         {GlassFamily::None, GlassFamily::Raised, GlassFamily::Surface, GlassFamily::Recessed}) {
        if (obj_has_style(obj, m.of(f), LV_PART_MAIN)) {
            return f;
        }
    }
    return std::nullopt;
}

GlassFamily main_family_from_styles(const lv_obj_t* obj) {
    for (StyleRole role :
         {StyleRole::ButtonGhost, StyleRole::ButtonTransparent, StyleRole::ButtonOutline}) {
        if (has_role(obj, role, LV_PART_MAIN)) {
            return GlassFamily::None;
        }
    }
    for (StyleRole role : {StyleRole::Button, StyleRole::ButtonPrimary, StyleRole::ButtonSecondary,
                           StyleRole::ButtonTertiary, StyleRole::ButtonDanger,
                           StyleRole::ButtonSuccess, StyleRole::ButtonWarning}) {
        if (has_role(obj, role, LV_PART_MAIN)) {
            return GlassFamily::Raised;
        }
    }
    if (has_role(obj, StyleRole::Card, LV_PART_MAIN) ||
        has_role(obj, StyleRole::Dialog, LV_PART_MAIN)) {
        return GlassFamily::Surface;
    }
    return GlassFamily::None;
}

/// Event user data: whether glass_attach() set the draw-task flag itself.
constexpr uintptr_t kFlagWasOurs = 1;
constexpr uintptr_t kFlagWasThere = 2;

void on_draw_task(lv_event_t* e) {
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    if (task == nullptr) {
        return;
    }
    auto* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(task));
    if (base == nullptr || base->obj == nullptr) {
        return;
    }
    lv_obj_t* obj = base->obj;
    const auto part = static_cast<lv_part_t>(base->part);
    const GlassFamily family = glass_family_of(obj, part);
    if (family == GlassFamily::None) {
        return;
    }
    const bool dark = ThemeManager::instance().is_dark_mode();
    switch (lv_draw_task_get_type(task)) {
    case LV_DRAW_TASK_TYPE_FILL:
        glass_rewrite_fill(lv_draw_task_get_fill_dsc(task), family, dark,
                           lv_obj_has_state(obj, LV_STATE_PRESSED));
        break;
    case LV_DRAW_TASK_TYPE_BORDER:
        glass_rewrite_border(lv_draw_task_get_border_dsc(task), family,
                             lv_obj_get_style_bg_color(obj, part), dark);
        break;
    default:
        break;
    }
}

lv_event_dsc_t* find_glass_event(const lv_obj_t* obj) {
    auto* o = const_cast<lv_obj_t*>(obj);
    const uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(o, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == on_draw_task) {
            return dsc;
        }
    }
    return nullptr;
}

} // namespace

GlassFamily glass_family_of(const lv_obj_t* obj, lv_part_t part) {
    if (obj == nullptr) {
        return GlassFamily::None;
    }
    const std::optional<GlassFamily> marked = marked_family(obj);
    if (marked == GlassFamily::None) {
        return GlassFamily::None;
    }
    if (part == LV_PART_MAIN) {
        return marked ? *marked : main_family_from_styles(obj);
    }
    return GlassFamily::None;
}

void glass_mark(lv_obj_t* obj, GlassFamily family) {
    Markers& m = markers();
    for (GlassFamily f :
         {GlassFamily::None, GlassFamily::Raised, GlassFamily::Surface, GlassFamily::Recessed}) {
        if (f != family) {
            lv_obj_remove_style(obj, m.of(f), LV_PART_MAIN);
        }
    }
    if (!obj_has_style(obj, m.of(family), LV_PART_MAIN)) {
        lv_obj_add_style(obj, m.of(family), LV_PART_MAIN);
    }
}

void glass_attach(lv_obj_t* obj) {
    if (obj == nullptr || lv_obj_check_type(obj, &lv_label_class) || find_glass_event(obj)) {
        return;
    }
    const bool had_flag = lv_obj_has_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    if (!had_flag) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }
    lv_obj_add_event_cb(obj, on_draw_task, LV_EVENT_DRAW_TASK_ADDED, // DECLARATIVE_OK: draw hook
                        reinterpret_cast<void*>(had_flag ? kFlagWasThere : kFlagWasOurs));
    lv_obj_invalidate(obj);
}

void glass_detach(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_event_dsc_t* dsc = find_glass_event(obj);
    if (dsc == nullptr) {
        return;
    }
    const auto how = reinterpret_cast<uintptr_t>(lv_event_dsc_get_user_data(dsc));
    lv_obj_remove_event_cb(obj, on_draw_task);
    if (how == kFlagWasOurs) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }
    lv_obj_invalidate(obj);
}

bool glass_attached(const lv_obj_t* obj) {
    return obj != nullptr && find_glass_event(obj) != nullptr;
}

void glass_rewrite_fill(lv_draw_fill_dsc_t* dsc, GlassFamily f, bool dark, bool pressed) {
    if (dsc == nullptr || dsc->opa != LV_OPA_COVER || dsc->grad.dir != LV_GRAD_DIR_NONE) {
        return;
    }
    glass_gradient(&dsc->grad, f, dsc->color, dark, pressed);
}

void glass_rewrite_border(lv_draw_border_dsc_t* dsc, GlassFamily f, lv_color_t base, bool dark) {
    if (dsc == nullptr || f == GlassFamily::None) {
        return;
    }
    dsc->color = glass_rim_color(f, base, dark);
}

} // namespace helix::ui
