set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(-Dcpp_std={{cpp-std}} --prefix "{{install_prefix}}")
    case "{{m}}" in
      release)
        args+=(--buildtype=release -Db_lto=true)
        ;;
      asan)
        args+=(--buildtype=debug -Db_sanitize=address)
        ;;
      debug|*)
        args+=(--buildtype=debug)
        ;;
    esac
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
    fi

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}}

debug: (build "debug")

asan: (build "asan")

release: (build "release")

install: (build "release")
    sudo meson install -C build-release

uninstall:
    sudo meson compile -C build-release uninstall

# Meson test suite: unit tests. Integration scripts in tests/*.sh need an Umbriel build and run separately.
test m=mode: (_ensure-configured m)
    meson compile -C build-{{m}}
    meson test -C build-{{m}} --print-errorlogs

# C++ sources and tests, all under the root .clang-format.
format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

# clang-tidy over src and tests, or only the given files: `just lint`, `just lint src/dbus/screencast.cpp`. Headers are
# checked through the sources that include them. Another build directory is `mode=`, as in `just mode=release lint`.
# -Wno-error keeps clang-only compiler warnings out of clang-tidy's errors.
[no-exit-message]
lint *files: (_ensure-configured mode)
    #!/usr/bin/env bash
    set -euo pipefail
    meson compile -C build-{{mode}}
    opts=(-quiet -p "build-{{mode}}" -header-filter='\.\./(src|tests)/.*' -warnings-as-errors='*' -extra-arg=-Wno-error)
    if (($# > 0)); then
        exec clang-tidy --use-color "${opts[@]}" "$@"
    fi
    run-clang-tidy -use-color -j "$(nproc)" "${opts[@]}" "^($(realpath src)|$(realpath tests))/.*"

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
