#!/bin/sh
# Fetch Lua 5.4.7 source into _deps/lua so the workshop plugin can build.
# Run once after cloning the repo. Idempotent.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEPS="$HERE/_deps"
LUA_VERSION="5.4.7"
LUA_TARBALL="lua-${LUA_VERSION}.tar.gz"
LUA_URL="https://www.lua.org/ftp/${LUA_TARBALL}"

if [ -f "$DEPS/lua/src/lua.h" ]; then
    echo "Lua $LUA_VERSION already present at $DEPS/lua"
    exit 0
fi

mkdir -p "$DEPS"
cd "$DEPS"

echo "Downloading $LUA_URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -sLO "$LUA_URL"
elif command -v wget >/dev/null 2>&1; then
    wget -q "$LUA_URL"
else
    echo "Need curl or wget to download Lua source" >&2
    exit 1
fi

tar xf "$LUA_TARBALL"
rm -rf lua
mv "lua-${LUA_VERSION}" lua
rm -f "$LUA_TARBALL"

echo "Lua $LUA_VERSION extracted to $DEPS/lua"
