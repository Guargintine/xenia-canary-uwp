# Building the UWP / Xbox application

Use Visual Studio 2022 with the C++ UWP tools, a Windows 10/11 SDK, CMake 3.21
or newer, and Python 3.10 or newer. The frontend targets x64 and Windows
10 version 1803 (17134) or newer. SDK C++/WinRT projections are used directly.

From the repository root:

```powershell
git submodule update --init --recursive
cmake --preset uwp
cmake --build --preset uwp-release -- /m:3 /p:CL_MPCount=2
```

The UWP solution is `build/uwp/xenia.sln`, with `xenia-uwp` as its startup
project. The executable and package outputs are beneath `build/uwp`. Keep
desktop configurations in another build directory because every UWP library
uses the app-container API partition.

The package includes the Blades frontend assets and the shared netplay dialogs.
Networking capabilities cover internet servers and private LAN connections.
The UWP HTTPS backend uses wolfSSL with the Windows certificate store and
BCrypt randomness; desktop builds retain Schannel. Desktop Discord IPC and USB
portal drivers are unavailable in the app container.

The packaged wolfSSL dependency uses its upstream GPLv2-or-later license;
its license text is included under `Licenses/wolfSSL/COPYING`.

Package signing is disabled by default so compilation does not depend on a
personal signing certificate. Configure your own matching package identity and
certificate in Visual Studio when creating a package for installation.

After installing, verify controller navigation, launching and returning from a
game, profile dialogs, and a multiplayer session on the intended Xbox or UWP
device. A host compilation alone does not validate those device interactions.
