#!/usr/bin/env bash
# Match build-violet.sh; pin the mirror to a checkout with real binaries.
set -euo pipefail
repo=https://github.com/halogenOS/android_prebuilts_clang_host_linux-x86_clang-r487747c.git
revision=19f5a09ce1b016b21a6ead1ed5d84c816586ebb6
toolchain="${RUNNER_TEMP:?}/clang-r487747c"
git init -q "$toolchain"
git -C "$toolchain" fetch -q --depth=1 "$repo" "$revision"
git -C "$toolchain" checkout -q --detach FETCH_HEAD
version=$("$toolchain/bin/clang" --version)
printf '%s\n' "$version"
grep -Fq 'based on r487747c' <<< "$version"
"$toolchain/bin/ld.lld" --version
printf '%s\n' "$toolchain/bin" >> "${GITHUB_PATH:?}"
