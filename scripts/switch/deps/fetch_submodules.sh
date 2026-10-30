#!/bin/bash
# 按 ppsspp-nx 记录的精确 commit 拉取所需子模块（走 codeload tarball，避免 git TLS 不稳）
set -u
cd /f/SWITCH-psp/ppsspp-nx || exit 1

fetch() {
    local path="$1" url="$2" sha="$3"
    local owner repo tmp
    owner=$(echo "$url" | sed -E 's#https://github.com/([^/]+)/(.*)#\1#')
    repo=$(echo "$url" | sed -E 's#https://github.com/([^/]+)/(.*)#\2#')
    repo=${repo%.git}
    if [ ! -d "$path" ] || [ -z "$(ls -A "$path" 2>/dev/null)" ]; then
        echo ">>> $path <- $owner/$repo@${sha:0:10}"
        rm -rf "$path" && mkdir -p "$path"
        tmp=$(mktemp /tmp/sub.XXXXXX.tgz)
        if curl -fsSL --retry 3 --max-time 300 \
             "https://codeload.github.com/$owner/$repo/tar.gz/$sha" -o "$tmp"; then
            tar -xzf "$tmp" -C "$path" --strip-components=1 && echo "    OK" || echo "    EXTRACT FAIL"
        else
            echo "    DOWNLOAD FAIL"
        fi
        rm -f "$tmp"
    else
        echo ">>> $path already populated, skip"
    fi
}

while read -r path url sha; do
    [ -z "$path" ] && continue
    fetch "$path" "$url" "$sha"
done <<'LIST'
ext/armips https://github.com/Kingcom/armips.git 62adab4ef30da765f5cf22a451eb08a59c54dc8b
ext/glslang https://github.com/hrydgard/glslang.git 50e0708ec3a5c16020c4f845c654b80b8edb80bd
ext/SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross.git 4212eef67ed0ca048cb726a6767185504e7695e5
ext/rapidjson https://github.com/Tencent/rapidjson.git 73063f5002612c6bf64fe24f851cd5cc0d83eef9
ext/zstd https://github.com/facebook/zstd.git f8745da6ff1ad1e7bab384bd1f9d742439278e99
ext/cpu_features https://github.com/google/cpu_features.git fd4ffc1632db7b4e763bd28ffa6fc9d761cf3587
ext/rcheevos https://github.com/RetroAchievements/rcheevos.git 2ac45d357bce2906bb0f1438f3eaf8ce6e78e3c4
ext/libchdr https://github.com/rtissera/libchdr.git 8bba7745d758627258b315997a860039244cedaf
ext/lua https://github.com/hrydgard/ppsspp-lua.git 7648485f14e8e5ee45e8e39b1eb4d3206dbd405a
ext/freetype https://github.com/hrydgard/freetype.git 063ed27f481f0bcd3962fd468e1546739d7d285a
ext/nanosvg https://github.com/hrydgard/nanosvg.git 478dbb8f7ed11c3d9b20b3986a8ee2283f483ef7
LIST
echo "=== DONE ==="
