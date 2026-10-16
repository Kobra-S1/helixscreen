// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_op_heater.h
 * @brief The nozzle heat before a filament op the runout dialogs dispatch.
 *
 * Load, Unload and Purge need a hotend above Klipper's min_extrude_temp, and the
 * runout guidance dialog that asks can be closed, or its owner destroyed, before
 * the nozzle gets there. So the wait belongs to an app-lifetime service, not to
 * the dialog.
 *
 * Two layers:
 *   - decide_preheat(): the pure rule. Hot enough, or heat to what and wait for what.
 *   - FilamentOpHeater: owns the one pending preheat, watches the nozzle, runs the
 *     op once it is hot, and schedules the post-op cooldown of heat it sent.
 *     Owned by SubjectInitializer, reached through get_filament_op_heater().
 *
 * The heater heats the active extruder. It draws nothing: its toasts are the
 * only heating UI the dialogs have.
 */

#pragma once

#include "ui_observer_guard.h"

#include "filament_op_dispatch.h"
#include "filament_op_slot_resolver.h"
#include "print_lifecycle_state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <vector>

class AmsBackend; // NAMESPACE_OK: forward declaration of the global AmsBackend

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::ui {

/// What a filament op does with the melt zone. Picks the slot whose material sets
/// the temperature, and the copy in log lines.
enum class FilamentHeatOp { Load, Unload, Purge };

/// Short lowercase name for @p op, for log lines. Never null.
[[nodiscard]] const char* filament_heat_op_name(FilamentHeatOp op);

/// A preheat is reached, and a hot nozzle counts as hot, within this many degrees.
inline constexpr int PREHEAT_MARGIN_C = 5;

/// What the nozzle reads now and what the op needs, in whole degrees.
struct PreheatInputs {
    int current_c = 0;             ///< Live temperature
    int target_c = 0;              ///< Live target; 0 when the heater is off
    std::optional<int> material_c; ///< What the op's material wants, when anything names it
    int held_c = 0;                ///< Latched last non-zero target (the swap latch)
    int floor_c = 0;               ///< temperature::extrusion_floor_c()
    int ceiling_c = 0;             ///< temperature::nozzle_max_temp_c()
    /// A target someone set and the nozzle has reached counts as hot enough.
    /// True for a purge, which pushes what is already in the melt zone, and for
    /// any op while a job holds the machine: a paused print's reload runs at the
    /// print's own temperature.
    bool honor_live_target = false;
};

/// What to do about @p PreheatInputs.
struct PreheatDecision {
    bool heat = false; ///< false: the nozzle is hot enough, run the op now
    int send_c = 0;    ///< The target to command (the controller keeps a hotter one)
    int wait_c = 0;    ///< The temperature the op waits for, within PREHEAT_MARGIN_C
};

/**
 * @brief The preheat rule, with no printer attached.
 *
 * Nothing runs below the extrusion floor. Above it, the nozzle is hot enough when
 *   - it is within PREHEAT_MARGIN_C of the hotter of the material's temperature
 *     and the swap latch, so the filament already in the melt zone still flows
 *     (with no material named, the floor stands in for it), or
 *   - @p honor_live_target is set and it has reached a target someone set.
 *
 * Otherwise it heats to the material's temperature, or DEFAULT_LOAD_PREHEAT_TEMP
 * when nothing names one, held between the floor and the ceiling, and waits for
 * that raised to the latch: the controller's keep_previous_hot holds the nozzle
 * there, so a lower wait would fire before the nozzle settles.
 */
[[nodiscard]] PreheatDecision decide_preheat(const PreheatInputs& in);

/// Whether @p current_c has reached a preheat waiting for @p wait_c: within the
/// margin, and never below the extrusion floor.
[[nodiscard]] bool preheat_reached(int current_c, int wait_c, int floor_c);

/// Why an op runs with no preheat from us.
enum class PreheatSkip {
    None,
    UserOverride,     ///< The allow-cold-extrude safety setting
    BackendSelfHeats, ///< The AMS backend heats the hotend for this op itself
};

[[nodiscard]] const char* preheat_skip_name(PreheatSkip reason);

/**
 * @brief Whether @p op, about to run as @p plan, needs our preheat.
 *
 * The cold-extrude override skips it on every tier. On the backend tier a load
 * (or swap) skips it when the backend claims supports_auto_heat_on_load(), or
 * on a tool changer, whose SELECT_TOOL owns its own heat sequence; an unload
 * skips it when AmsBackend::unload_heats_hotend() says so for that slot. Macro
 * and raw-gcode tiers always preheat: nothing tells us a macro heats.
 *
 * @param plan Unset for an op with no tier plan (purge).
 */
[[nodiscard]] PreheatSkip preheat_skip_reason(FilamentHeatOp op, const FilamentOpPlan* plan,
                                              AmsBackend* backend);

/// Names one request, so its completion can say whether the heater heated for it.
/// 0 is no request.
using FilamentOpTicket = uint64_t;

/// One filament op that needs a hot nozzle.
struct PreheatRequest {
    FilamentHeatOp op = FilamentHeatOp::Load;

    /// Prefixes log lines. Static storage duration.
    const char* log_tag = "[Filament]";

    /// Anything but None runs the op with no heat.
    PreheatSkip skip = PreheatSkip::None;

    /// Who asked: a static tag. With @p op and @p slot it names the request, so
    /// the same request again is a re-tap.
    const void* owner = nullptr;

    /// The lane whose material sets the temperature. Negative for none.
    int slot = -1;

    /// The op. Runs on the main thread, now when no heat is needed, else once hot.
    /// It is handed the request's ticket, which its completion passes to
    /// op_succeeded() / op_failed(). Must outlive the surface: capture nothing a
    /// closing dialog frees.
    std::function<void(FilamentOpTicket)> run;
};

/**
 * @brief The one pending preheat, and the cooldown after a filament op it heated for.
 *
 * Main thread only. One preheat at a time, because there is one nozzle:
 *   - the SAME request again (op, slot and owner) while it waits is the user
 *     changing their mind, so it cancels the pending one and runs nothing;
 *   - another op from the same owner is refused with a toast: the dialog stays
 *     open while the nozzle heats, and its first op must not be dropped;
 *   - a request from another owner replaces it, and takes over the heat it sent.
 *
 * A preheat ends without running its op when the target it sent never lands,
 * when that target is taken away, when it runs past PREHEAT_TIMEOUT, for a
 * request made while no job held the machine when a job takes it, and for one
 * made during a pause when the pause ends (resumed, cancelled, finished). One
 * that times out, is cancelled or outlives its print turns the nozzle back off
 * if it was off when the request arrived; a target someone else set stays.
 * One whose target was taken away, or that a running print overtook, leaves
 * the heater to whoever moved it.
 */
class FilamentOpHeater {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// How long a sent target may take to appear on the nozzle.
    static constexpr std::chrono::seconds TARGET_LAND_TIMEOUT{20};
    /// How long a preheat may take altogether.
    static constexpr std::chrono::minutes PREHEAT_TIMEOUT{10};
    /// How long a backend op may run before its completion stops being watched.
    static constexpr std::chrono::minutes BACKEND_WATCH_TIMEOUT{15};

    /// @param clock Steady time; unset reads std::chrono::steady_clock.
    explicit FilamentOpHeater(PrinterState& state, Clock clock = {});
    ~FilamentOpHeater();

    FilamentOpHeater(const FilamentOpHeater&) = delete;
    FilamentOpHeater& operator=(const FilamentOpHeater&) = delete;

    enum class Outcome {
        Ran,       ///< No heat needed (or skipped); run() has returned
        Heating,   ///< run() waits for the nozzle
        Cancelled, ///< A re-tap withdrew the pending request; run() never runs
        Refused,   ///< The same owner's other op still waits; run() never runs
    };

    /// Run @p req.run now, or heat first and run it once hot.
    Outcome run_after_preheat(PreheatRequest req);

    /// Whether a preheat is waiting.
    [[nodiscard]] bool pending() const {
        return pending_.has_value();
    }

    /// Withdraw the pending preheat: the nozzle goes back off if it was off when
    /// the request arrived, and the op never runs. False when nothing was pending.
    bool cancel();

    /// The op @p ticket names finished. Schedules the post-op cooldown when this
    /// heater heated for it and no job holds the machine.
    void op_succeeded(FilamentOpTicket ticket);

    /// The op @p ticket names failed: the nozzle stays where the user can see
    /// what happened, and the ticket is forgotten.
    void op_failed(FilamentOpTicket ticket);

    /// A backend op for @p ticket is about to be dispatched and completes on the
    /// AMS action feed: op_succeeded() when the action moves to busy after this
    /// call and then returns to IDLE, op_failed() on ERROR, forgotten after
    /// BACKEND_WATCH_TIMEOUT. Arm it BEFORE the dispatch so a fast backend cannot
    /// finish unseen. Does nothing for a ticket the heater did not heat for.
    void watch_backend_completion(FilamentOpTicket ticket);

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Pending {
        PreheatRequest req;
        FilamentOpTicket ticket = 0;
        int wait_c = 0;
        int floor_c = 0;
        int prior_target_c = 0; ///< The nozzle target before we sent ours
        bool target_seen = false;
        bool job_free_at_request = true;
        PrintState lifecycle_at_request = PrintState::Idle;
        TimePoint started{};
    };

    struct Heated {
        FilamentOpTicket ticket = 0;
        TimePoint at{};
    };

    struct Watch {
        FilamentOpTicket ticket = 0;
        int last_action = -1; ///< The action as it stood when last seen
        bool seen_busy = false;
        TimePoint deadline{};
    };

    void start_heating(Pending p, int send_c, const std::string& material_name);
    void on_temperature();
    void on_target(int target_deci);
    void on_lifecycle();
    /// Whether the print has moved on from where it stood at the request: any
    /// job for an idle request, leaving the pause for a paused one.
    [[nodiscard]] bool print_moved_on() const;
    void on_ams_action(int action);
    void check_deadlines();
    /// Take the pending preheat out and stop watching the nozzle, then run its op
    /// when @p run: the op runs with the heater already idle, so it may start
    /// another request.
    void end_pending(bool run);
    /// End the pending preheat unrun: toast, put the target back when @p restore.
    void abandon(const char* why, const char* toast, bool restore);
    /// Turn the nozzle back off when @p prior_target_c says it was off.
    void restore_prior(int prior_target_c);
    void cancel_deadline_timer();
    [[nodiscard]] TimePoint now() const;
    [[nodiscard]] int read_c(lv_subject_t* subject) const;
    [[nodiscard]] bool heated_for(FilamentOpTicket ticket) const;
    void forget(FilamentOpTicket ticket);

    PrinterState& state_;
    Clock clock_;
    std::optional<Pending> pending_;
    FilamentOpTicket next_ticket_ = 0;
    std::vector<Heated> heated_;
    std::vector<Watch> watches_;
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard action_observer_;
    lv_timer_t* deadline_timer_ = nullptr;
};

/// The app's FilamentOpHeater (a shared resource SubjectInitializer registers),
/// or nullptr before it exists.
[[nodiscard]] FilamentOpHeater* get_filament_op_heater();

/// Hand @p req to the app's heater; with no heater, run it now with no preheat.
void run_with_preheat(PreheatRequest req);

/// Report the end of the op @p ticket names to the app's heater, from any
/// thread: the report is marshalled to the main thread.
void report_filament_op_done(FilamentOpTicket ticket, bool succeeded);

/// The material a filament op on @p slot heats for: that lane's, else the
/// external spool's (resolve_load_preheat_material()). nullopt when neither names
/// one, so DEFAULT_LOAD_PREHEAT_TEMP applies.
[[nodiscard]] std::optional<PreheatTarget> preheat_material_for_slot(int slot);

} // namespace helix::ui
