<!-- source_hash: 7bdf40b6e01e -->
# krkrz_tools — Kirikiri Z standard tools

[日本語](README.md) | English

The standard tool set for Kirikiri Z (吉里吉里Z). It replaces the standard tools of the Kirikiri 2 era
(built with C++Builder, Windows only).

- Runs on **Windows / Linux / macOS** (on macOS only Intel has been confirmed; Apple Silicon has not been confirmed)
- Each tool is a **native app that uses the browser as its screen** ([appserve](https://github.com/wamsoft/appserve)).
  On startup it runs a local HTTP server and opens its screen in a browser app window
- **Every tool also runs from the command line** (`--cli`). The GUI and the CLI call the same processing
- Compatible with the formats of the old tools (public keys, `.sig`, signatures embedded in the exe, ini files and so on)

The plan and progress are in [TODO-tools.md](https://github.com/wamsoft/krkrz_dev/blob/develop/TODO-tools.md)
of krkrz_dev (in Japanese).

## Tool list

The documents under `docs/` are in Japanese.

| Executable | Tool | Status | Details |
|---|---|---|---|
| `krkrcheck` | File corruption checker | Confirmed working (Windows) | [docs/krkrcheck.md](docs/krkrcheck.md) |
| `krkrsign` | Key generation and signing tool | Confirmed working (Windows) | [docs/krkrsign.md](docs/krkrsign.md) |
| `krkrxp3` | xp3 archive tool (create, list, extract, verify) | Confirmed working (Windows) | [docs/krkrxp3.md](docs/krkrxp3.md) |
| `krkraudio` | Sound format converter (conversion, volume, volume levels for lip sync) | Confirmed working (Windows) | [docs/krkraudio.md](docs/krkraudio.md) |
| `krkrloop` | Loop tuner (edit .sli links and labels while viewing the waveform; loop playback by the same rules as the engine) | Confirmed working (Windows) | [docs/krkrloop.md](docs/krkrloop.md) |
| `krkrrelease` | Releaser (makes the exe for distribution: embedded options, icon, version information, security settings, xp3, signature. **Windows only**) | Confirmed working (Windows) | [docs/krkrrelease.md](docs/krkrrelease.md) |
| `krkrimg` | Image format converter (successor to the old krkrtpc; compositing and layer export of PSD / CLIP) | Confirmed working (Windows) | [docs/krkrimg.md](docs/krkrimg.md) |

Every tool opens its screen when started with no arguments (or with a file or folder), and processes
without opening the screen when given `--cli`. The commands and options of each tool are described in
the docs/ page in the table above.

### krkrcheck — File corruption checker

Verifies, with a public key, the files that have a `.sig` under a folder and the Kirikiri executables
with an embedded signature. The public key and the screen text are read from «executable name.ini»
(the Kirikiri 2 «ファイル破損チェックツール.ini» can be used as is). The CLI exit codes are 0 = all OK /
1 = corruption found / 2 = some files could not be verified, or failure.
→ [docs/krkrcheck.md](docs/krkrcheck.md)

### krkrsign — Key generation and signing tool

Creates RSA keys (`keygen`), signs (`sign`) and verifies (`verify`). Signatures are SHA256 + RSA-PSS;
for a Kirikiri executable the signature goes into the exe, otherwise into «file name.sig». Compatible
with the keys and signatures of the old krkrsign.
→ [docs/krkrsign.md](docs/krkrsign.md)

### krkrxp3 — xp3 archive tool

Creates (`pack`), lists (`list`), extracts (`extract`) and verifies (`verify`) xp3 archives. It writes
the same format with the same default classification as the Kirikiri 2 releaser (krkrrel), and reads
`default.rpf` (the releaser profile). Encryption and Ogg Vorbis codebook sharing are not supported.
→ [docs/krkrxp3.md](docs/krkrxp3.md)

### krkraudio — Sound format converter

Converts between WAV / Ogg Vorbis / Ogg Opus (`convert`), handles volume (gain, loudness
normalization, ReplayGain), and exports volume levels for lip sync (`lipsync`). «file.sli» (loop
information) is carried over to the converted file as well.
→ [docs/krkraudio.md](docs/krkraudio.md)

### krkrloop — Loop tuner

Edits the links and labels of «sound file.sli» while viewing the waveform, and plays the loop by the
same rules as the engine's `WaveLoopManager`. From the CLI it can show the contents of a .sli (`info`)
and check that the engine can read it (`check`).
→ [docs/krkrloop.md](docs/krkrloop.md)

### krkrrelease — Releaser (Windows only)

Makes the exe for distribution from a Kirikiri Z executable (the WINVER build / the SDL build on
Windows). The settings are saved in `release.json`, and it processes the embedded options, icon,
version information, security settings, data (xp3 placed next to the exe / bound to it) and signature
in that order.
→ [docs/krkrrelease.md](docs/krkrrelease.md)

### krkrimg — Image format converter

Reads BMP / PNG / JPEG / TLG5 / TLG6 / PSD / CLIP STUDIO (.clip) and converts by the same rules as the
old krkrtpc (`convert`). It can also export the layers of a PSD / CLIP one by one (`layers`). Its TLG
output is byte-identical to the engine's.
→ [docs/krkrimg.md](docs/krkrimg.md)

## Common options

- `--cli` … processes without opening the screen, then exits
- Options that take a value are written in the form **`--name=value`** (`--key=public.txt`)
- `--help` shows the list. `--port=N` / `--browser=none` and so on are appserve startup options (see `--help`)

## Requirements

- CMake 3.21 or later
- Ninja
- [vcpkg](https://github.com/microsoft/vcpkg) — set the environment variable `VCPKG_ROOT` to where it is installed
- Windows: Visual Studio 2022 (run from its Developer Command Prompt)

## Build

```bash
git submodule update --init
cmake --preset windows          # linux / macos (Apple Silicon) / macos-x64 (Intel)
cmake --build --preset windows-rel
```

The output is `build/<preset>/tools/<tool>/Release/<tool>.exe`. The screen (web/) is embedded in the
exe, so a single exe is all you need. On Windows the MSVC runtime and the vcpkg dependency libraries
are linked statically (triplet `x64-windows-static`).

Confirmed environments (2026-10-09): Windows x64 (VS 2022), Linux x64 (with the Steam Linux Runtime 3.0
«sniper» SDK: GLIBC 2.30 or lower, no libstdc++ dependency; building directly on the host also works),
macOS x64 (Intel, `macos-x64`). Apple Silicon (`macos`) has not been confirmed.

Setting the environment variable `KRKRZ_BASE` to the parent of the folder that holds krkrz_dev lets the
build reference the Kirikiri Z engine sources (the same convention as krkrz_android / krkrz_linux). No
tool uses the engine sources directly yet, so the build also works without it.

## Layout

```
external/appserve   Browser UI framework (submodule)
external/psdparse   PSD loading and compositing (submodule)
external/clipparse  CLIP STUDIO (.clip) loading and compositing (submodule; fetches sqlite with FetchContent)
cmake/KrtTool.cmake krt_add_tool(): the tool exe + embedding of the screen
libs/app            Common frame (GUI / CLI dispatch, running long jobs with progress, path conversion)
libs/sig            Digital signatures (verification, key generation, signing)
libs/xp3            xp3 archive reading and writing
libs/loop           Loop information (.sli) reading and writing
libs/audio          Sound reading and writing (WAV / Vorbis / Opus), loudness and volume
libs/tlg            TLG5 / TLG6 reading and writing (port of the engine's SaveTLG5/6 and LoadTLG)
libs/image          Image reading and writing (BMP / PNG / JPEG / TLG / PSD / CLIP), the same preprocessing as the old krkrtpc, layer export
libs/release        Makes the exe for distribution (PE resource rewriting, security settings, binding, signing; Windows only)
web/common          Screen parts shared by all tools (krt.js / krt.css: folder and file pickers and more)
tools/<tool>/       main.cpp (CLI + API) and web/ (screen)
docs/               Description of each tool
```

## License

This repository is under the same terms as the Kirikiri Z engine ([LICENSE](LICENSE)). Parts of the
TLG, .sli and xp3 reading and writing are ported from the engine code. The submodules and the libraries
fetched through vcpkg follow their own licenses.
