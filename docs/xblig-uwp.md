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
  - The user mode alias space uses `VirtualAlloc2FromApp` and
    `MapViewOfFile3FromApp`, like `MapFileView` already does in the app
    container.
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

The storage root is the app's `LocalState` folder (Device Portal, File
explorer, `LocalAppData\XeniaXBLIG.Experimental_<id>\LocalState`).

1. `Xna_TitleLauncher.xex` from the Xbox 360 system update (USB update
   package, `$SystemUpdate`). Put it in a folder of its own, because that
   folder is mounted as `game:`. Either:
   - `LocalState\XNA\`, or
   - an `XNA` folder next to the indie games, e.g. `E:\Indie\XNA\`, or
   - any path, set as `xna_host_path` in the config.
2. The XNA runtime title update `tu32000100_00000000` for 584E07D2, dumped
   from your own console. Install it once: select any game in the library,
   Manage DLC, pick the file. It lands in
   `LocalState\content\0000000000000000\584E07D2\000B0000`.
3. Indie game packages go in a game folder like any other title. Launching
   one installs it as content and boots the launcher with it.

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
