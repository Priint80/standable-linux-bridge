#pragma once

#include <chrono>
#include <thread>

// The controller ray already moves Wine's real pointer and produces hover
// states. The remaining failure is activation: focus can land on an internal
// render child and a press/release pair can be flushed without Wine observing
// the press first. These narrowly-scoped call-site shims keep the existing
// capture implementation untouched while making button delivery synchronous.
template <typename SyncFunction, typename Display>
inline int standable_dashboard_sync_button(
    SyncFunction sync,
    Display* display,
    int pressed) {
    sync(display, 0);
    if (pressed != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return 1;
}

// This macro expands only at the existing XTestFakeButtonEvent call. The real
// XTEST result remains part of the boolean expression; synchronization runs
// only after XTEST reports success.
#define test_fake_button_event(display, button, pressed, delay) \
    test_fake_button_event(display, button, pressed, delay) && \
    standable_dashboard_sync_button(api_.sync, display, pressed)

// Capture may switch to a Wine child render surface. Pointer coordinates still
// target that child, but keyboard/mouse activation should focus the top-level
// application window so Wine dispatches the click through its normal path.
#define set_input_focus(display, target, revert, time) \
    set_input_focus( \
        display, \
        (top_level_target_ != 0UL ? top_level_target_ : target), \
        revert, \
        time)
