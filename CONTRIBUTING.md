# Contributing

xdg-desktop-portal-umbriel shares its conventions with [Umbriel](https://github.com/noctalia-dev/umbriel/blob/main/CONTRIBUTING.md): same code style, clang-format and clang-tidy configuration, contribution rules, and commit format. If in doubt, match what Umbriel does.

## Development Commands

The README covers routine builds and installation. Contributor checks use:

| Command                 | Purpose                                                                                      |
| ----------------------- | -------------------------------------------------------------------------------------------- |
| `just configure <mode>` | Create or reconfigure a build directory and symlink `compile_commands.json` to it            |
| `just test`             | Build and run the Meson test suite                                                           |
| `just lint [file ...]`  | Build, then run clang-tidy on `src` and `tests` or only the given files; warnings are errors |
| `just format`           | Format C++ sources and tests                                                                 |

CI runs the format, build and test, and lint checks on Arch Linux with clang 23. `.clang-format` and `.clang-tidy` are copies of Umbriel's; change them in Umbriel first and copy them here.

Run `lefthook install` to install the pre-commit hook. It runs `just format` when C++ files are staged.

## Code Style

Formatting uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) (LLVM base, 2-space indent, 120 columns, left pointer alignment, regrouped includes, braces around every control statement body). Run `just format` before committing.

Static analysis uses [clang-tidy](https://clang.llvm.org/extra/clang-tidy/). Run `just lint` (warnings are errors). Prefer the modern idioms the checks enforce: `auto`, ranges, `std::print`/`std::format`, `make_unique`, scoped locks, no C-style casts, uppercase literal suffixes (`1.0F`).

Domain-oriented C++23: one domain per directory under `src/` (`dbus/`, `wayland/`, `pipewire/`, `picker/`, ...), headers beside their sources, and `src/` as the include root. Headers use `#pragma once`.

### Naming Conventions

|                        | Convention           | Example                        |
| ---------------------- | -------------------- | ------------------------------ |
| Files                  | snake_case           | `input_capture.cpp`            |
| Directories            | snake_case           | `dbus/`, `pipewire/`           |
| Types / Classes        | PascalCase           | `ScreenCastPortal`             |
| Functions / Methods    | camelCase            | `startCaptures()`              |
| Variables / Parameters | camelCase            | `chooserCmd`                   |
| Private members        | m_camelCase          | `m_impl`                       |
| Constants              | k-prefixed constexpr | `kRestoreDataVersion`          |
| Enumerators            | PascalCase           | `Session::DynamicMode::Manual` |
| Macros                 | SCREAMING_SNAKE_CASE | `XDPU_DEFAULT_CHOOSER_CMD`     |

clang-format inserts braces around every control statement body, and clang-tidy enforces the naming table for enumerators and private members.

Scoped enums (`enum class`) use an explicit `std::uint8_t` underlying type where it makes sense.

Getters are the noun, without a `get` prefix: `appId()`, `selections()`, `dynamicMode()`.

Use `[[nodiscard]]` only where ignoring the result is likely a bug: a call with side effects that reports failure or hands back something the caller must own, consume or keep. Getters and pure functions go without it.

### Configuration vocabulary

|                                     | Convention | Example                              |
| ----------------------------------- | ---------- | ------------------------------------ |
| TOML keys                           | snake_case | `chooser_cmd`, `max_fps`             |
| Identifiers owned by another system | verbatim   | output `DP-1`, `app_id`, D-Bus names |

### Comments and documentation

Comments and documentation explain what the code currently does and any non-obvious constraint a reader needs. Do not narrate history, migrations, or rejected alternatives; git holds that.

## Tests

Unit tests are Meson tests in `tests/` and run with `just test`. They cover pure logic such as capture transforms and PipeWire buffer parameters.

Behaviour that needs a running compositor is covered by integration scripts that take an Umbriel build directory and this repository's build directory:

```sh
tests/screencast_transform.sh ../umbriel/build-debug build-debug
tests/screencast_pipewire_restart.sh ../umbriel/build-debug build-debug
```

Each starts an isolated headless Umbriel, a private D-Bus session and PipeWire daemon, and the portal, with a scripted chooser in place of the share picker. The InputCapture checks live in the Umbriel repository; see [InputCapture verification](README.md#inputcapture-verification).

## Share chooser protocol

`screencast.chooser_cmd` receives the available sources as one JSON object on stdin and answers with one line of JSON on stdout:

```json
{ "selections": [{ "kind": "monitor", "output": "DP-1" }], "dynamic": false }
```

A window selection is `{"kind": "window", "identifier": "<ext-foreign-toplevel identifier>"}`. `dynamic` is optional and only honoured for a single selection; see [Changeable screencast targets](README.md#changeable-screencast-targets). A non-zero exit cancels the request.
