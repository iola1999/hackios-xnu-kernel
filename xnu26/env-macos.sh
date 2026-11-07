# source this (from bash) before building the xnu-12377 kernel on macOS (xnu26/build-macos.sh)
XNU26_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export XNU26_ROOT
if [ -z "${XNU26_SRC:-}" ]; then
    if [ -d "$XNU26_ROOT/xnu-src/xnu-12377.121.6" ]; then
        export XNU26_SRC="$XNU26_ROOT/xnu-src/xnu-12377.121.6"
    else
        export XNU26_SRC="$XNU26_ROOT/out/xnu12377-cmd"
    fi
fi
export XNU26_DEPS=${XNU26_DEPS:-$XNU26_ROOT/xnu-deps/26}
export XNU26_SDK=$XNU26_DEPS/sdk/iPhoneOS.sdk
