// Copyright 2026 Xenia Canary. Released under the BSD license.

#include "xenia/hid/dsu/dsu_input_driver.h"

#include "xenia/base/cvar.h"

DEFINE_bool(dsu_enabled, false, "Enable the remote DSU controller.", "DSU");
DEFINE_string(dsu_host, "127.0.0.1", "IPv4 address of the DSU server.", "DSU");
DEFINE_int32(dsu_port, 26760, "UDP port of the DSU server.", "DSU");
DEFINE_int32(dsu_remote_slot, 0, "Remote controller slot (zero-based).", "DSU");
DEFINE_int32(dsu_local_player, 0, "Xbox controller slot (zero-based, 0 to 3).",
             "DSU");
// Preserve the configuration key used by existing installations.
DEFINE_bool(dsu_retrodsu, true,
            "Enable companion control with compatible button mapping and "
            "registration. Disable for standard DSU servers.",
            "DSU");

DEFINE_bool(dsu_player_profiles, false, "Use per-player DSU configuration.",
            "DSU");

#define DEFINE_DSU_PLAYER(n)                                                   \
  DEFINE_bool(dsu_player##n##_enabled, false, "Enable DSU player " #n ".",     \
              "DSU");                                                          \
  DEFINE_string(dsu_player##n##_host, "127.0.0.1", "DSU server IPv4 address.", \
                "DSU");                                                        \
  DEFINE_int32(dsu_player##n##_port, 26760, "DSU server UDP port.", "DSU");    \
  DEFINE_int32(dsu_player##n##_slot, 0, "DSU controller slot.", "DSU");        \
  DEFINE_bool(dsu_player##n##_extensions, true,                                \
              "Enable companion extensions.", "DSU");                          \
  DEFINE_bool(dsu_player##n##_motion_right_stick, false,                       \
              "Add DSU motion to the right stick.", "DSU");                    \
  DEFINE_double(dsu_player##n##_motion_sensitivity_x, 1.0,                     \
                "Horizontal motion sensitivity multiplier.", "DSU");           \
  DEFINE_double(dsu_player##n##_motion_sensitivity_y, 1.0,                     \
                "Vertical motion sensitivity multiplier.", "DSU");             \
  DEFINE_double(dsu_player##n##_motion_deadzone, 0.5,                          \
                "Motion deadzone in degrees per second.", "DSU");              \
  DEFINE_bool(dsu_player##n##_motion_invert_x, false,                          \
              "Invert horizontal motion.", "DSU");                             \
  DEFINE_bool(dsu_player##n##_motion_invert_y, false,                          \
              "Invert vertical motion.", "DSU")
DEFINE_DSU_PLAYER(1);
DEFINE_DSU_PLAYER(2);
DEFINE_DSU_PLAYER(3);
DEFINE_DSU_PLAYER(4);
#undef DEFINE_DSU_PLAYER

namespace xe::hid::dsu {

Configuration DSUInputDriver::ReadConfiguration(uint32_t player) {
  if (!cvars::dsu_player_profiles) {
    if (player != uint32_t(cvars::dsu_local_player)) {
      Configuration config;
      config.local_player = player;
      return config;
    }
    return {cvars::dsu_enabled,
            cvars::dsu_host,
            uint32_t(cvars::dsu_port),
            uint32_t(cvars::dsu_remote_slot),
            uint32_t(cvars::dsu_local_player),
            cvars::dsu_retrodsu};
  }
#define READ_DSU_PLAYER(n)                                         \
  case n - 1:                                                      \
    return {                                                       \
      cvars::dsu_player##n##_enabled, cvars::dsu_player##n##_host, \
          uint32_t(cvars::dsu_player##n##_port),                   \
          uint32_t(cvars::dsu_player##n##_slot), player,           \
          cvars::dsu_player##n##_extensions,                       \
          cvars::dsu_player##n##_motion_right_stick,               \
          float(cvars::dsu_player##n##_motion_sensitivity_x),      \
          float(cvars::dsu_player##n##_motion_sensitivity_y),      \
          float(cvars::dsu_player##n##_motion_deadzone),           \
          cvars::dsu_player##n##_motion_invert_x,                  \
          cvars::dsu_player##n##_motion_invert_y                   \
    }
  switch (player) {
    READ_DSU_PLAYER(1);
    READ_DSU_PLAYER(2);
    READ_DSU_PLAYER(3);
    READ_DSU_PLAYER(4);
    default:
      return {};
  }
#undef READ_DSU_PLAYER
}

void DSUInputDriver::SaveConfigurations(
    const std::array<Configuration, 4>& players) {
  const auto set = []<typename T>(const std::string& name, T value) {
    auto* option =
        dynamic_cast<cvar::ConfigVar<T>*>(cvar::ConfigVars->at(name));
    option->OverrideConfigValue(value);
  };
  for (uint32_t i = 0; i < players.size(); ++i) {
    const auto& player = players[i];
    const auto prefix = "dsu_player" + std::to_string(i + 1) + "_";
    set(prefix + "enabled", player.enabled);
    set(prefix + "host", player.host);
    set(prefix + "port", int32_t(player.port));
    set(prefix + "slot", int32_t(player.remote_slot));
    set(prefix + "extensions", player.companion_extensions);
    set(prefix + "motion_right_stick", player.motion_right_stick);
    set(prefix + "motion_sensitivity_x", double(player.motion_sensitivity_x));
    set(prefix + "motion_sensitivity_y", double(player.motion_sensitivity_y));
    set(prefix + "motion_deadzone", double(player.motion_deadzone));
    set(prefix + "motion_invert_x", player.motion_invert_x);
    set(prefix + "motion_invert_y", player.motion_invert_y);
  }
  set("dsu_player_profiles", true);
}

}  // namespace xe::hid::dsu
