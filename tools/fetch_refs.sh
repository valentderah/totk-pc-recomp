#!/bin/sh
# Clone the upstream projects this port builds on into refs/, at pinned commits. refs/ is read-only reference
# material and ignored by git.
set -eu

cd "$(dirname "$0")/.."
mkdir -p refs

# fetch <name> <url> <commit> [history]: a shallow checkout of the commit, or with "history" the commit and all its
# ancestors (tools/shader_list.py walks the log of MacNativeGames' shader list). origin/HEAD names the commit either
# way, so tools can read the pinned revision as origin/HEAD.
fetch() {
    name=$1 url=$2 commit=$3 history=${4:-}
    dir=refs/$name
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ] &&
       [ "$(git -C "$dir" rev-parse -q --verify refs/remotes/origin/HEAD 2>/dev/null)" = "$commit" ] &&
       { [ -z "$history" ] || [ "$(git -C "$dir" rev-parse --is-shallow-repository)" = false ]; }; then
        echo "$name: up to date"
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    if [ -n "$history" ]; then
        git -C "$dir" fetch -q origin "$commit"
    else
        git -C "$dir" fetch -q --depth 1 origin "$commit"
    fi
    git -C "$dir" checkout -q FETCH_HEAD
    git -C "$dir" update-ref refs/remotes/origin/HEAD "$commit"
    echo "$name: $commit"
}

fetch LASRecomp      https://github.com/peiikai/LASRecomp.git        a6b4f03844ecfb4012f18deff96da8b410aeafcd
fetch MacNativeGames https://github.com/ly05010419/MacNativeGames.git d1509b4b62211265fad5449cf53cc9f13484ecc5 history
fetch d2r-mobile     https://github.com/jhaizhou-ops/d2r-mobile.git  fae8c680fe98f79179cf3fab85f7a6c14ee28600
fetch nxnative       https://github.com/seedds/nxnative.git          99330eb81c873c3aafe9fc92194c9687d77fcdbb
fetch SwitchRecomp   https://github.com/bgyss/SwitchRecomp.git       0f99e1c5c1e78fdc8e01e58a0f8dc8622e82976b
fetch deko3d           https://github.com/devkitPro/deko3d.git         373bfd43d8b2d805fa2eb7d406dbb823032df958
fetch SavageWindRecomp https://github.com/Revivedx/SavageWindRecomp.git bf6146c14c37fee8781a601e5a544b93dff6d75e
