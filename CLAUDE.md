# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

`krkrz_tools` is the standard tool set for 吉里吉里Z (Kirikiri Z), replacing the Kirikiri2-era tools
(krdevui.dll + krkrrel / krkrsign / krkrtpc / krkrlt …, C++Builder, Windows only). The plan and progress
live in the umbrella repo: `krkrz_dev/TODO-tools.md` (SSOT — update it when a phase / tool lands).

Every tool is a native executable whose GUI is a browser window ([appserve](external/appserve): localhost
HTTP + SSE + browser app-mode window). **Every tool must also run headless from the command line**
(`--cli`), and the GUI and CLI must call the same processing code — never put logic only in the GUI.

## Layout

- `external/appserve` — submodule (wamsoft/appserve). Zero external deps, C++17. Read its `CLAUDE.md` /
  `docs/DESIGN.md` before changing how tools use it. Do not fork behaviour into this repo; fix appserve upstream.
- `cmake/KrtTool.cmake` — `krt_add_tool(<target> SOURCES … WEB_DIR … LIBS …)`. Stages
  `appserve/web/lib` + `web/common` + the tool's `web/` into `build/…/tools/<tool>/web` at configure
  time (source files are CMAKE_CONFIGURE_DEPENDS, so edits re-stage on the next build) and embeds it.
- `libs/app` (`krt_app`) — `ToolApp` (adds `--cli`, `/api/app/info`, fs module, JobRunner),
  `JobRunner` (one background job; `/api/job`, `/api/job/cancel`, SSE `job` / `joblog`),
  `Progress` (the only interface processing code talks to; `ConsoleProgress` for CLI), `Text.h`
  (UTF-8 ⇔ `std::filesystem::path`; **always** convert through `krt::toPath` / `krt::fromPath`).
- `libs/sig` (`krt_sig`) — signatures (libtomcrypt / libtommath from vcpkg).
- `libs/xp3` (`krt_xp3`) — xp3 read / write (zlib from vcpkg).
- `web/common` — shared UI (`krt.js`: init, job watching, folder / file picker; `krt.css`).
- `tools/<name>/` — `main.cpp` (options, CLI path, `IModule` with the tool's API) + `web/`.

## Build

```bash
cmake --preset windows            # linux / macos; needs VCPKG_ROOT
cmake --build --preset windows-rel
```

Windows: run from a VS 2022 Developer prompt (PowerShell: `Enter-VsDevShell` for
`C:\Program Files\Microsoft Visual Studio\2022\Professional`). MSVC runtime and vcpkg triplet are
static (`x64-windows-static`) so each tool ships as one exe.

`KRKRZ_BASE` (env) = folder containing `krkrz_dev`. Tools that reuse engine sources (xp3 / TLG / .sli …)
must reference them from there instead of copying them — same convention as krkrz_android / krkrz_linux.

## Compatibility rules (do not break)

- Formats must stay compatible with the Kirikiri2 tools **and** with the engine-side readers:
  - signatures: SHA256 + RSA-PSS, `.sig` text format, exe embedded signature at RELEASE_SIG mark + 16 + 4,
    hash excludes OPT_EMBED_AREA … xp3 start (or EOF). The engine verifier is
    `krkrz_dev/src/plugins/sigcheck/main.cpp`; any change in `libs/sig` must keep giving the same
    result as that plugin. Cross-check by running the engine with a startup script that calls
    `Window.checkSignature` (see "Testing").
  - keys: RSA PKCS#1 PEM (`rsa_export` **without** `PK_STD`), 64-column lines, CRLF.
  - xp3: must stay readable by the engine reader `krkrz_dev/src/core/common/base/XP3Archive.cpp`
    (header mark, I64 index pointer, index flag 0 raw / 1 zlib / 0x80 continue, `File` → `info` /
    `segm` (28-byte records) / `adlr` chunks, UTF-16LE names, bit31 = protected). We write the
    krkrrel «cushion» header. Cross-check by reading `<arc>.xp3>name` from a krkrz64 startup script
    (e.g. `Storages.getMD5HashString`) and by reading an xp3 made by the legacy `krkrrel.exe`.
- Legacy test vectors: `kirikiri2/tests/sigcheck/` in the Kirikiri2 repo (an old private key and its `.sig`).

## Conventions

- Option values are passed as `--name=value` (appserve's parser; `--name value` does not work).
- CLI exit codes: 0 = success, 1 = check found problems (broken etc.), 2 = error / could not process.
- Long work runs through `JobRunner::start` with a `Progress&`; handlers that may block use
  `Affinity::Any` or a job — never block the main-thread handler queue.
- UI strings, comments and docs are Japanese; source is UTF-8. Keep the style of existing files.
- **No project/client-identifying information** (titles, client names, private repo names, issue keys)
  anywhere in this repo, including commit messages. This repo is meant to be shared.

## Testing

No automated test suite yet. Verify each tool both ways:

- CLI: run `<tool> --cli …` against a scratch folder and check output / exit code.
- GUI: start with `--browser=none --port=<N> --idle-timeout=0`, open the printed URL in a browser,
  drive it, then stop the process **by PID** (never by image name — other sessions may be running tools).
- Engine cross-check (signatures): copy `sigcheck.dll` next to a krkrz64 build, run it with a
  `startup.tjs` that calls `Window.checkSignature(file, publicKey, info)` and logs `onCheckSignatureDone`.
