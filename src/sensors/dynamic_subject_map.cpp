// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynamic_subject_map.h"

#include <algorithm>

namespace helix::sensors {

DynamicSubjectMap::~DynamicSubjectMap() {
    clear();
}

lv_subject_t* DynamicSubjectMap::get_or_create(const std::string& key, int initial,
                                               SubjectLifetime& out_lifetime) {
    auto& slot = slots_[key];
    if (!slot) {
        slot = std::make_unique<Slot>();
        lv_subject_init_int(&slot->subject, initial);
        slot->lifetime = std::make_shared<bool>(true);
    }
    out_lifetime = slot->lifetime;
    return &slot->subject;
}

lv_subject_t* DynamicSubjectMap::find(const std::string& key, SubjectLifetime& out_lifetime) const {
    const auto it = slots_.find(key);
    if (it == slots_.end()) {
        out_lifetime.reset();
        return nullptr;
    }
    out_lifetime = it->second->lifetime;
    return &it->second->subject;
}

void DynamicSubjectMap::remove_missing(const std::vector<std::string>& keep) {
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) {
            release(*it->second);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void DynamicSubjectMap::clear() {
    for (auto& [key, slot] : slots_) {
        release(*slot);
    }
    slots_.clear();
}

void DynamicSubjectMap::release(Slot& slot) {
    // Every outstanding copy of the token reads dead before the observers go.
    if (slot.lifetime) {
        *slot.lifetime = false;
    }
    slot.lifetime.reset();
    if (lv_is_initialized()) {
        lv_subject_deinit(&slot.subject);
    }
}

} // namespace helix::sensors
