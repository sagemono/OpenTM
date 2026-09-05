# OpenTM

An independent target manager for the PlayStation 3, speaking the DECI3 protocol over TCP.

OpenTM connects to a console, browses its filesystem, inspects kernel objects, loads and runs executables from your PC, installs packages, and edits the console's debug settings - the day-to-day work a target manager does, without needing the original tooling running.

There is a GUI, a headless CLI for build steps and CI, and a small server that
holds sessions so several tools can drive one console at once.

![The OpenTM window connected to a DECR-1000A devkit, with the kernel explorer listing a running process's threads and modules, and TTY output in the console below](assets/app.png)

```sh
# run what you just built on a devkit, and fail the build if it asserts
opentm_cli --host 192.0.2.10 --serve build --reset --load build/game.self \
           --pass-on "RESULT: OK" --fail-on "ASSERT|FATAL"
```

## Status

Working against real hardware:

| | DECR devkit (`PS3_DEH_TCP`) | Retail/Nonretail systems running DEX firmware (`PS3_DBG_DEX`) |
|---|---|---|
| Connect / session handshake | yes | yes |
| TTY console | yes | yes |
| File explorer (browse, stat) | yes | yes |
| Host file serving (`/app_home/`) | yes | yes |
| Load and run a .self | yes | yes |
| Install a `.pkg` | yes | yes |
| Kernel explorer | yes | yes |
| Power control / reset modes | yes | yes |
| Upload / download | yes | yes |
| Rename, mkdir, delete on the console | yes | yes |
| Permissions and timestamps | yes | not supported |
| XMB settings read/write | yes | yes |
| Wake-on-LAN | n/a - the CP powers the system on | yes |

`PS3_CORE_DUMP` targets are recognised but not currently implemented.

## Getting it



Builds are on the [releases page](../../releases). On Windows, unzip and run `opentm_app.exe`. On Linux, either the `.AppImage` (one file, `chmod +x` and run) or the `.tar.gz` (unpack anywhere, run `bin/opentm_app`). All of them carry their own Qt, so nothing needs installing.

To build from source, see [Building](docs/building.md).

## Documentation

| | |
|---|---|
| [Usage](docs/usage.md) | the GUI, the CLI, and how a console is shared |
| [Reporting a problem](docs/reporting.md) | what to include so a bug can be diagnosed |
| [Building](docs/building.md) | building from source on Windows and Linux |
| [Internals](docs/internals.md) | how the code is put together, and where to add things |
| [Server protocol](docs/rpc-protocol.md) | driving OpenTM from your own tools |

## Reporting a problem

Open an [issue](../../issues) - please not chat, since these usually take more than one round trip and a thread that is still there next week is worth a lot. [Reporting a problem](docs/reporting.md) covers what to include; the short version is the wire log (right-click the Wire Log dock, Save to File), your target type and port, and whether the official tool does the same thing.

## Relationship to vendor tooling

OpenTM is a cleanroom implementation built from protocol observation. It is not affiliated with, endorsed by, or derived from Sony Interactive Entertainment or its subsidiaries, including SN Systems Ltd.

A few features invoke executables that ship with the official SDK. OpenTM does not bundle those, it expects you to point it at your own copy, and disables the feature when the file is absent.

"PlayStation", "PS3" and "ProDG" are trademarks of their respective owners, used here only to identify the hardware and software OpenTM interoperates with.

## Attributions

**FamFamFam Silk icon set** by Mark James - <http://www.famfamfam.com/lab/icons/silk/> 

Licensed under [Creative Commons Attribution 2.5][ccby25].

[ccby25]: https://creativecommons.org/licenses/by/2.5/

## Licence

OpenTM is licensed under MIT
