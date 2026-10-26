# UWP Netplay integration

The source head before rebasing was `f9ed10f332e7d56c2f3bd16e591b93164428366e` (`Blades-UWP`). Both it and `UWP-Netplay` pointed to that exact commit before the operation.

The new base is AdrianCassar/xenia-canary `netplay_canary_experimental`, commit `26dea033c7c2aa19e509b2c04ad712b2bb9d88e1` (2026-09-07), fetched on 2026-09-08.

The 27 source commits belong in their existing parent-to-child order. Author timestamps survive earlier rebases and are not monotonic: the foundation commit is dated 2026 while many dependent commits retain 2023-2024 dates, and two README changes have reversed author dates. Sorting by author date would break ancestry and dependencies. The original author dates and final source head are preserved in this record.

## Original commit sequence

| Order | Original commit | Author date | Subject |
| --- | --- | --- | --- |
| 1 | `3c58e61f1` | 2026-02-25T23:40:15Z | Initial changes for canary and foundation for UI |
| 2 | `5637d3b1b` | 2023-02-11T16:30:47Z | Temporarily force 1080p until the scaling issue is resolved. |
| 3 | `42abbc930` | 2023-02-11T16:34:46Z | Retropass Support & DPI Fix |
| 4 | `c7ef4c39b` | 2023-02-11T16:40:36Z | Fix mount_cache and some defaults |
| 5 | `6155691f6` | 2023-02-17T00:01:45Z | imgui game list and settings menu |
| 6 | `f838d77b7` | 2023-02-17T12:37:41Z | On-Screen Keyboard for XAM UI |
| 7 | `434c5139a` | 2023-02-17T17:00:38Z | Fix for retropass support with the frontend |
| 8 | `0f6241e9d` | 2023-02-19T19:00:37Z | Re-enable resolution scaling, and use DXGI stretch to workaround the zoom bug. |
| 9 | `c98b297b4` | 2023-02-20T20:59:48Z | Improve name resolution for frontend. Read title from GOD and check default.xex in a case-insensitive way. |
| 10 | `7e17cff61` | 2023-02-20T22:53:12Z | Extra settings and settings UI improvements |
| 11 | `01f167b4c` | 2023-04-17T14:41:23+01:00 | Frontend improvements, CL and DLC options, improved keyboard dialog |
| 12 | `0169e2bf3` | 2023-04-17T16:52:41+01:00 | Alert user if selected folders aren't writable. |
| 13 | `1a785b0ae` | 2023-04-17T18:27:04+01:00 | Additional Content: Allow selecting multiple files + a 'installing' dialog |
| 14 | `c346e7c08` | 2023-04-18T13:41:09+01:00 | Update to latest revision, new settings menu |
| 15 | `b0ea2521c` | 2023-06-14T17:54:35+01:00 | Fix build errors |
| 16 | `ad32bca8b` | 2023-06-14T17:55:08+01:00 | Add version to About tab |
| 17 | `d8b90eb79` | 2023-06-14T18:21:04+01:00 | Add NotosansJP and Roboto fonts |
| 18 | `422b2d61f` | 2023-09-30T20:51:13+01:00 | Changes for 1.1.4 |
| 19 | `6129de360` | 2024-04-14T17:22:43+01:00 | Release 1.1.5 |
| 20 | `e718cf7f6` | 2024-04-21T11:05:21+01:00 | Add zar files to frontend, change version to 1.1.5a |
| 21 | `9015657e8` | 2024-04-21T11:43:14+01:00 | Better game scanner |
| 22 | `6aeb9a78f` | 2024-04-21T11:56:45+01:00 | fix |
| 23 | `f7244a8cf` | 2024-09-22T14:55:15+01:00 | Update README.md |
| 24 | `bb27e93c0` | 2024-09-13T15:50:03-07:00 | Update README.md |
| 25 | `b8b6b0085` | 2026-02-26T00:48:04Z | Fix merge conflict markers in emulator.cc |
| 26 | `6d01cd533` | 2026-02-26T19:28:14Z | WinRT rebase/refactors |
| 27 | `f9ed10f33` | 2026-05-01T12:12:17+01:00 | Blades UI |

## Integration decisions

- Preserve upstream CPU, GPU, netplay, profile, content-manager and dependency improvements. Keep the CMake build system and upstream submodule revisions.
- Keep the actual Blades frontend and guide-style ImGui windows for UWP. Retain current Xbox Live profile flags, gamerpic initialization and sign-in behavior behind those windows.
- Scope Xbox/UWP API restrictions and defaults to the UWP build so the desktop paths remain available.
- Historical orphaned UI code and literal merge markers are resolved against the functioning upstream implementation; the completed Blades frontend is applied last. Two redundant historical changes retain their position as empty commits.
- Retain the `xeniacanary:` Windows protocol and RetroPass command/return URI. Complete `launchOnExit` before upstream process teardown, and wait for emulator initialization before consuming a game path.
- Expose the shared netplay/profile windows during gameplay through the [View + Menu host guide](uwp-host-guide.md), with controller Back handling and deferred texture/owner cleanup.
- Use separate UWP and desktop build directories because their Windows API/ABI targets differ.

## Validation

- Visual Studio 2022 Release builds: desktop `xenia-app` and UWP `xenia-uwp` compiled and linked successfully. UWP unsigned x64 APPX packaging completed.
- APPX inspection: x64 App Container executable, correct entry point, all 20 Blades assets byte-identical to source, network/LAN capabilities and bundled TLS license present.
- Integrated controller/protocol tests: 34 assertions in 5 cases passed (23 protocol assertions plus 11 controller chord assertions).
- Desktop VFS tests: 27 assertions in 4 cases passed. The same ISO tests linked against the UWP VFS archive passed 23 assertions in 4 cases, exercising its file-backed read path on Windows.
- UWP-built libcurl/wolfSSL host probe: HTTPS request with native Windows certificate trust returned HTTP 200; malformed explicit CA trust was rejected.
- All 37 upstream submodule revisions are retained. No upstream source files were deleted, and generated shader bytecode matches upstream.

Build outputs are under `build/desktop/bin/Windows/Release` and `build/uwp/bin/Windows/Release`; the unsigned package is under `build/uwp/xenia-canary-uwp/AppPackages`. Use [the UWP build instructions](building-uwp.md) for deployment setup.

Host builds and tests do not validate Xbox app-container permissions, controller feel, a real RetroPass URI round-trip, or gameplay/netplay between devices. Those checks still require deployment to the intended device and a game.
