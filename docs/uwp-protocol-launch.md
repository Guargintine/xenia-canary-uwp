# UWP protocol launches

The UWP package retains the `xeniacanary:` Windows protocol used by RetroPass.
Its decoded `cmd` query value accepts a game path, optionally preceded by
`xeniacanary.exe`. Quote paths containing spaces. Executable names are
case-insensitive and may also be quoted, including a full executable path.

Examples of decoded `cmd` values:

```text
xeniacanary.exe "D:\Games\Halo 3\default.xex"
"XeniaCanary.exe" "D:\Games\Halo 3\default.xex"
"D:\Games\Halo 3\default.xex"
```

Encode query values when constructing the URI. For example:

```text
xeniacanary:?cmd=xeniacanary.exe%20%22D%3A%5CGames%5CHalo%203%5Cdefault.xex%22&launchOnExit=retropass%3A%2F%2F
```

Windows decodes the query before Xenia parses it. Backslashes and spaces inside
quotes are preserved. Extra command options and malformed quoting are rejected;
the protocol does not execute a shell command.

An optional `launchOnExit` URI is handed to Windows when Xenia closes. Each
protocol activation replaces this return URI, so a later activation without
`launchOnExit` does not return to an earlier frontend. Game paths are consumed
once, and launches wait for emulator initialization, including when
`skip_frontend` is enabled.

When a game requests another XEX, Xenia saves the requested module and launch
data and asks Xbox to restart the app automatically. The saved request takes
precedence over the original frontend game path. The frontend return URI is
carried in restart arguments and persisted for recovery if automatic restart
fails. It is restored only with a saved title request and consumed once.

An internal title restart does not launch the return URI. In-process disc swaps
also preserve it. Closing the game or Xenia normally returns to the original
frontend, including after multiple title restarts.
