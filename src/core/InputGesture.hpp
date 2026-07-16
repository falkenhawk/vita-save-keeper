#pragma once

namespace vsm {

enum class TapHoldAction {
  None,
  Tap,
  Hold,
};

// A quick release is a tap. Reaching the hold threshold fires once while the button is down;
// releasing after the tap window is an intentional back-out and does nothing.
constexpr TapHoldAction resolve_tap_hold_action(int held_frames, bool released, bool consumed,
                                                int tap_frames, int hold_frames) {
  if (consumed || held_frames <= 0) {
    return TapHoldAction::None;
  }
  if (!released && held_frames >= hold_frames) {
    return TapHoldAction::Hold;
  }
  if (released && held_frames < tap_frames) {
    return TapHoldAction::Tap;
  }
  return TapHoldAction::None;
}

} // namespace vsm
