// Copyright 2026 Xenia Canary. Released under the BSD license.

#include <array>
#include <string>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia-canary-uwp/protocol_activation.h"

namespace {

TEST_CASE("RetroPass command preserves the game path", "[uwp][protocol]") {
  const std::string path = R"(D:\Games\Halo 3\default.xex)";
  for (const auto& command : {
           "xeniacanary.exe \"" + path + "\"",
           "XeniaCanary.exe \"" + path + "\"",
           "\"xeniacanary.exe\" \"" + path + "\"",
           "\"C:\\Program Files\\Xenia\\XENIACANARY.EXE\" \"" + path + "\"",
           "\"" + path + "\"",
           "  \"" + path + "\"\t",
       }) {
    INFO(command);
    REQUIRE(UWP::ParseProtocolCommand(command) == path);
  }
  REQUIRE(UWP::ParseProtocolCommand(R"(D:\Games\default.xex)") ==
          R"(D:\Games\default.xex)");
  REQUIRE(UWP::ParseProtocolCommand("\"D:\\Games\\日本語\\default.xex\"") ==
          "D:\\Games\\日本語\\default.xex");
}

TEST_CASE("Protocol command rejects incomplete paths and extra options",
          "[uwp][protocol]") {
  for (const auto command : {
           "",
           " \t",
           "xeniacanary.exe",
           "\"\"",
           "xeniacanary.exe \"D:\\Games\\default.xex",
           "xeniacanary.exe \"D:\\Games\\default.xex\" --gpu=d3d12",
           "xeniacanary.exe --gpu=d3d12",
           "\"D:\\Games\\default.xex\"unexpected",
           "D:\\Games\\Halo 3\\default.xex",
       }) {
    INFO(command);
    REQUIRE_FALSE(UWP::ParseProtocolCommand(command).has_value());
  }
}

TEST_CASE("Protocol return URI belongs only to its activation",
          "[uwp][protocol]") {
  using Argument = std::pair<std::string, std::string>;
  const std::array first = {
      Argument{"cmd", R"(xeniacanary.exe "D:\Games\Halo 3\default.xex")"},
      Argument{"launchOnExit", "retropass://return?system=xbox360"},
  };
  auto activation = UWP::ParseProtocolActivation(first);
  REQUIRE(activation.game_path == R"(D:\Games\Halo 3\default.xex)");
  REQUIRE(activation.launch_on_exit == "retropass://return?system=xbox360");

  const std::array second = {
      Argument{"cmd", R"("D:\Games\Ridge Racer 6\default.xex")"},
      Argument{"unrelated", "ignored"},
  };
  activation = UWP::ParseProtocolActivation(second);
  REQUIRE(activation.game_path == R"(D:\Games\Ridge Racer 6\default.xex)");
  REQUIRE(activation.launch_on_exit.empty());

  activation = UWP::ParseProtocolActivation({});
  REQUIRE_FALSE(activation.game_path.has_value());
  REQUIRE(activation.launch_on_exit.empty());
}

}  // namespace
