// Copyright 2026 Xenia Canary. Released under the BSD license.

#include <array>

#include "third_party/catch/include/catch.hpp"
#include "xenia-canary-uwp/host_guide_input.h"

TEST_CASE("UWP host guide chord triggers once until every pad releases",
          "[uwp][host-guide]") {
  UWP::HostGuideChord chord;
  std::array<uint16_t, 4> buttons{};
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[2] = UWP::HostGuideChord::kButtons;
  REQUIRE(chord.Poll(buttons) == 2);
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[0] = UWP::HostGuideChord::kButtons;
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[2] = 0;
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[0] = 0;
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[3] = UWP::HostGuideChord::kButtons;
  REQUIRE(chord.Poll(buttons) == 3);
}

TEST_CASE("UWP host guide chord does not combine buttons across pads",
          "[uwp][host-guide]") {
  UWP::HostGuideChord chord;
  std::array<uint16_t, 4> buttons{0x0020, 0x0010, 0, 0};
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[1] |= 0x0020;
  REQUIRE(chord.Poll(buttons) == 1);
  buttons.fill(0);  // Also represents all controllers disconnecting.
  REQUIRE_FALSE(chord.Poll(buttons));
  buttons[1] = UWP::HostGuideChord::kButtons;
  REQUIRE(chord.Poll(buttons) == 1);
}
