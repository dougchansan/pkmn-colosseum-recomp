#pragma once
#include <algorithm>

// Do not hold emulation behind its real-time clock for inserted video frames.
// Hysteresis avoids switching modes for ordinary timer/presentation jitter.
constexpr bool frame_interpolation_catchup(bool active, double lag_seconds) {
  return lag_seconds > (active ? 1.0 / 240.0 : 1001.0 / 30000.0);
}

// rate=0 is native; the simulation remains 59.94 fields/s. Game frames can
// span one field (cutscenes) or two (gameplay), so requested host FPS alone
// cannot determine the replay count. Aurora supports at most four layers.
constexpr unsigned frame_interpolation_layers(unsigned rate, bool allowed, unsigned fields) {
  fields = std::clamp(fields, 1u, 2u);
  return allowed && rate ? std::clamp((rate * fields + 59) / 60, 1u, 4u) : 1u;
}
