# Sailfish OS target

Builds a Gea JSX/CSS app into a local Sailfish OS RPM. The host runs Vite and
geatsc; `sfdk` compiles the staged C/C++ sources for Sailfish OS 5.1.0.11.

Requirements: Node.js 20.19+, `npm install` in this Linux repository and in
the app's workspace, a checkout of `geastack/core`, and Sailfish SDK with the
selected build target. The default core checkout is `../core` relative to this
Linux repository. Pass `--core-repo` (Bash) or `-CoreRepository` (PowerShell)
for another location.

From the Linux repository on Linux:

```sh
./targets/sailfish-os/build-sailfish-os.sh ../examples/apps/tic-tac-toe --arch i486
```

The script finds `sfdk` on `PATH`. Pass `--sfdk /path/to/SailfishOS/bin/sfdk`
if it is elsewhere. `--prepare-only` runs code generation and stages the
portable project without invoking `sfdk`.

From the Linux repository on Windows:

```powershell
./targets/sailfish-os/build-sailfish-os.ps1 -AppDirectory ../examples/apps/tic-tac-toe -Architecture i486
```

Use `-SdkRoot` when Sailfish SDK is installed outside `C:\SailfishOS`.

The RPM is written to
`targets/sailfish-os/build/<app-id>-<architecture>/project/RPMS/`.
Use `-PrepareOnly` in PowerShell for code generation without invoking `sfdk`.

The Windows script invokes the installed `sfdk.exe` with session handling off.
The SDK build engine must be running. On this host, invoking `sfdk.exe` inside
MSYS2 fails to identify its Docker engine, while the PowerShell invocation
works. MSYS2 remains installed for the SDK's other command-line workflows.

The Sailfish target shares SDL2 rendering, touch, storage, and network sources
with the Raspberry Pi OS target. Its compile definition selects a scale of 1
and a fullscreen window. The i486 RPM was installed on the Sailfish OS
5.1.0.11 emulator; Tic Tac Toe rendered and mouse clicks changed the board.
Physical device behavior has not been tested.
