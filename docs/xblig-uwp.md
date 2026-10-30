# Xbox Live Indie Games on the UWP / Xbox build

Experimental. This branch runs Xbox Live Indie Games (XBLIG, XNA Creators
Club packages) in the UWP app for Xbox One / Series consoles in Developer Mode.

## What is in it

- Base: danprice142/xenia-canary-uwp `Blades-UWP-Netplay` at `2e030620`.
- XBLIG support from TylerJaacks/xenia-canary `xblig_support_alpha`
  (`8d91fe46`, `49a2883c`): kernel user mode (KeEnterUserMode and friends),
  guest generated code for the .NET Compact Framework JIT, a user mode alias
  address space, XNA package detection and the XNA title launcher handoff.
- Upstream Xenia Canary fixes that Tyler's tree already had and this base did
  not: the AllocRange ceiling fix, the vec128 context promotion revert and the
  related HIR/x64 vector fixes, guest dispatch header and object type fixes,
  XamContentFlush and content fixes, missing XAM exports, and the storage
  device notification fix.
- UWP changes:
  - The user mode alias space falls back to `VirtualAlloc2FromApp` and
    `MapViewOfFile3FromApp` in builds without the desktop or games API
    partitions. Current SDKs expose the desktop functions to UWP games (the
    GAMES partition), so this build uses `VirtualAlloc2` and `MapViewOfFile3`,
    like the rest of Xenia's memory code.
  - The launcher is looked for in `<storage root>\XNA\Xna_TitleLauncher.xex`
    and in an `XNA` folder next to the indie game package, not only in the
    working directory (read only on UWP).
  - The game library lists indie game packages (marketplace content owned by
    584E07D2 or 584E07D1).
  - Own package identity (`XeniaXBLIG.Experimental`, publisher
    `CN=XeniaXBLIG`, protocol `xeniaxblig:`) so it installs beside other Xenia
    builds and keeps its own LocalState.
  - The build generates the sideloading `.appx` on every build.

## Setup on the console

Everything below can go on internal storage through Device Portal's File
explorer. The app's storage root is its `LocalState` folder:
`LocalAppData\XeniaXBLIG.Experimental_t2rrnbpbeykme\LocalState`.

1. `Xna_TitleLauncher.xex` from the Xbox 360 system update (USB update
   package, `$SystemUpdate`). Put it in a folder of its own, because that
   folder is mounted as `game:`. The app looks in `LocalState\XNA\`, then in
   an `XNA` folder next to the indie game package, e.g.
   `DevelopmentFiles\Games\xbox360\Indie\XNA\`. Setting `xna_host_path` in
   the config replaces both (a relative path resolves against the storage
   root).
2. The XNA runtime title update `tu32000100_00000000` for 584E07D2, dumped
   from your own console. Copy it to
   `LocalState\content\0000000000000000\584E07D2\000B0000\`, or install it
   with Manage DLC on any game if the file picker can reach it.
3. Indie game packages go in a game folder like any other title, e.g.
   `DevelopmentFiles\Games\xbox360\Indie\` with the Games directory set to
   `D:/DevelopmentFiles/Games/xbox360` in the app's settings (or a USB drive
   under the default `E:/Games/xbox360`). Launching one installs it as content
   and boots the launcher with it.

Config:

- Every indie game runs as title 584E07D2, so put
  `[Content]` `license_mask = -1` in `LocalState\config\584E07D2.config.toml`.
  Without it indie games stay in their 8 minute trial.
- `network_mode = 0` in `[Live]` of `LocalState\xenia-canary-netplay.config.toml`
  runs offline, the way the XBLIG work was tested.

## Known limits

- Faults in user mode code (for example managed null references) are not
  delivered to the XNA runtime yet; such a game crashes.
- Avatars are not emulated; games draw none.
- Tested upstream on PC with a handful of titles only. Nothing here has been
  run on a console before.
- `LocalState\xenia.log` shows `Xbox Live Indie Game detected`,
  `XNA title launcher:` and, on failures, the reason.
- When Xenia closes on its own, the log ends with why: a crash report between
  `====` lines (also kept in `LocalState\xenia-crash.txt` until the next
  crash), or the guest call that asked to close, with the calls before it.
  Each release has a symbols zip that turns the report's `xeniacanary+0x...`
  offsets into function names.
- On an Xbox Series S the console refuses guest memory and translated code
  at about 700 MiB together (`Host memory: committing ... failed with system
  error 1455`), far below the app's 5 GB, while it still reports gigabytes
  free. Xenia tries a refused request again in smaller pieces, then gives up.
  So on the Xbox `commit_all_physical_memory` (`[Memory]`) is off: Xenia gets
  guest physical memory as games allocate it, and gives a page memory when
  the GPU or a game first touches one no game allocated, instead of taking
  all 512 MiB at start.

## Building

`.github/workflows/xblig-uwp.yml` builds the package on GitHub Actions
(`windows-2022`, Visual Studio 2022 with the UWP C++ tools) on every push to
`xblig-netplay`, signs it with a throwaway `CN=XeniaXBLIG` certificate and
publishes it as a prerelease with its `Microsoft.VCLibs` dependency. The
desktop Orchestrator pipeline is manual only on this branch. To build locally,
see [building-uwp.md](building-uwp.md).
