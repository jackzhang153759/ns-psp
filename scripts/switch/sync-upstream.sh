#!/usr/bin/env bash
# 同步 PPSSPP 主线（hrydgard/ppsspp）到本 Switch 移植仓库，并可选地合并 + 编译验证。
#
# 用法：
#   bash scripts/switch/sync-upstream.sh                 # 默认 = --check，只检查有无新提交
#   bash scripts/switch/sync-upstream.sh --check         # 只拉取并报告落后多少提交（不改动代码）
#   bash scripts/switch/sync-upstream.sh --merge         # 拉取并把上游新提交合并进当前分支
#   bash scripts/switch/sync-upstream.sh --merge --build # 合并后自动编译验证
#
# 环境变量：
#   UPSTREAM_URL   上游仓库地址，默认 https://github.com/hrydgard/ppsspp
#   UPSTREAM_REF   上游分支，默认 master
#
# 设计要点（都是踩过坑才加的）：
#   1. 本仓库的 ext/aemu_postoffice 子模块损坏会让 `git status` / `git diff` 静默失效
#      （报 0 改动）。因此本脚本内部一律使用 --ignore-submodules=all。
#   2. 上游 tip 记录在 docs/switch/UPSTREAM-BASE，合并成功后自动更新。
#   3. 合并可能冲突；脚本不强行解决，而是把冲突文件列出来交给人/AI 处理。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BASE_FILE="$REPO_ROOT/docs/switch/UPSTREAM-BASE"
BUILD_SCRIPT="$SCRIPT_DIR/build.sh"

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/hrydgard/ppsspp}"
UPSTREAM_REF="${UPSTREAM_REF:-master}"
LOCAL_BRANCH="upstream-master"

DO_CHECK=0
DO_MERGE=0
DO_BUILD=0

for arg in "$@"; do
  case "$arg" in
    --check) DO_CHECK=1 ;;
    --merge) DO_MERGE=1 ;;
    --build) DO_BUILD=1 ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "未知参数: $arg（用 --help 查看用法）" >&2; exit 2 ;;
  esac
done

# 默认只检查
if [ "$DO_MERGE" -eq 0 ] && [ "$DO_BUILD" -eq 0 ]; then DO_CHECK=1; fi

cd "$REPO_ROOT" || exit 1

echo "=== PPSSPP 主线同步 ==="
echo "仓库:   $REPO_ROOT"
echo "当前分支: $(git rev-parse --abbrev-ref HEAD)"
echo "HEAD:   $(git rev-parse --short HEAD)"
echo

# ---------- 1. 准备上游 remote ----------
# 优先用已配置的 upstream（GitHub）；否则若存在本地镜像 hryd（离线快）就用它；
# 都没有才新增 GitHub remote。
fetch_upstream_to_local_branch() {
  local refspec="refs/heads/${UPSTREAM_REF}:refs/heads/${LOCAL_BRANCH}"

  if git remote get-url upstream >/dev/null 2>&1; then
    echo ">> 从 remote 'upstream' 拉取 $(git remote get-url upstream)"
    git fetch --no-tags upstream "$refspec" && return 0
  fi

  # 本地镜像（本机开发用）：注意必须取它的 origin/master，直接取 master 会拿到旧提交
  if git remote get-url hryd >/dev/null 2>&1; then
    echo ">> 从本地镜像 remote 'hryd' 拉取（取 refs/remotes/origin/${UPSTREAM_REF}）"
    git fetch --no-tags hryd "refs/remotes/origin/${UPSTREAM_REF}:refs/heads/${LOCAL_BRANCH}" && return 0
  fi

  echo ">> 新增 remote 'upstream' -> $UPSTREAM_URL"
  git remote add upstream "$UPSTREAM_URL" || return 1
  git fetch --no-tags upstream "$refspec"
}

echo "--- 步骤 1/4：拉取上游 ---"
if ! fetch_upstream_to_local_branch; then
  echo "✗ 拉取上游失败。检查网络，或设置 UPSTREAM_URL 指向可达的镜像。" >&2
  exit 1
fi
UPSTREAM_TIP="$(git rev-parse "$LOCAL_BRANCH")"
echo "上游 tip: $UPSTREAM_TIP  ($(git log -1 --format=%s "$LOCAL_BRANCH"))"
echo

# ---------- 2. 读取上次已合并的基线 ----------
BASE_COMMIT=""
if [ -f "$BASE_FILE" ]; then
  BASE_COMMIT="$(tr -d '[:space:]' < "$BASE_FILE")"
fi
if [ -z "$BASE_COMMIT" ] || ! git cat-file -e "$BASE_COMMIT^{commit}" 2>/dev/null; then
  BASE_COMMIT="$(git merge-base HEAD "$LOCAL_BRANCH" 2>/dev/null || echo '')"
fi

echo "--- 步骤 2/4：比对 ---"
if [ -z "$BASE_COMMIT" ]; then
  echo "⚠ 找不到已合并的上游基线，无法计算落后数量。将用 merge-base 结果代替。"
  AHEAD=0
else
  echo "上次已合并的上游基线: $(git rev-parse --short "$BASE_COMMIT")"
  AHEAD="$(git rev-list --count "$BASE_COMMIT..$LOCAL_BRANCH" 2>/dev/null || echo 0)"
fi
echo "落后上游: $AHEAD 个提交"
echo

if [ "$AHEAD" -eq 0 ]; then
  echo "✓ 已与上游同步，无需操作。"
  [ "$DO_BUILD" -eq 1 ] && echo "（仍按 --build 要求执行编译验证）" || exit 0
fi

# ---------- 3. 检查（默认）----------
if [ "$DO_CHECK" -eq 1 ] && [ "$DO_MERGE" -eq 0 ]; then
  echo "--- 步骤 3/4：--check 模式，不改动代码 ---"
  if [ "$AHEAD" -gt 0 ]; then
    echo "上游新增提交（最新 20 个）："
    git log --oneline --no-decorate "$BASE_COMMIT..$LOCAL_BRANCH" | head -n 20
    echo
    echo "→ 要合并请运行： bash scripts/switch/sync-upstream.sh --merge"
  fi
  exit 0
fi

# ---------- 4. 合并 ----------
echo "--- 步骤 3/4：合并上游到当前分支 ---"

# 合并前确认工作区没有未提交的改动（子模块损坏会让裸 git status 失效，必须加参数）
if [ -n "$(git diff --ignore-submodules=all --name-only)" ]; then
  echo "⚠ 工作区有未提交改动，建议先提交或暂存："
  git diff --ignore-submodules=all --name-only | head -n 20
  echo "（继续合并可能产生难以拆分的冲突。Ctrl-C 可中止）"
fi

if ! git merge --no-ff --no-commit "$LOCAL_BRANCH"; then
  echo
  echo "✗ 合并出现冲突。冲突文件："
  git diff --name-only --diff-filter=U | tee /tmp/ppsspp-conflicts.txt
  echo
  echo "处理建议："
  echo "  1. 逐个解决冲突（保留我们的 Switch 适配与 XMB/UMD 改动，见 docs/switch/our-changes.md）"
  echo "  2. git add <file> 标记已解决"
  echo "  3. git commit 完成合并后，再运行本脚本 --check 确认基线已更新"
  exit 3
fi

# 无冲突：直接提交（若 --no-commit 后暂存区为空说明无需合并）
if git diff --cached --quiet --ignore-submodules=all; then
  echo "✓ 合并后无实际变化，取消合并。"
  git merge --abort 2>/dev/null || true
else
  git commit -m "Merge upstream hrydgard/ppsspp $(git rev-parse --short "$LOCAL_BRANCH") into the Switch port" \
    || { echo "✗ 提交失败" >&2; exit 4; }
  echo "✓ 合并已提交。新 HEAD: $(git rev-parse --short HEAD)"
fi

# 更新基线记录
git rev-parse "$LOCAL_BRANCH" > "$BASE_FILE"
echo "✓ 已更新基线记录 $BASE_FILE -> $(cat "$BASE_FILE")"
echo

# ---------- 5. 编译验证 ----------
if [ "$DO_BUILD" -eq 1 ]; then
  echo "--- 步骤 4/4：编译验证 ---"
  if [ ! -f "$BUILD_SCRIPT" ]; then
    echo "✗ 找不到编译脚本 $BUILD_SCRIPT" >&2; exit 5
  fi
  bash "$BUILD_SCRIPT"
  rc=$?
  if [ $rc -eq 0 ]; then
    echo "✓ 合并后编译验证通过。"
  else
    echo "✗ 合并后编译失败（exit $rc）。常见原因是交叉编译适配问题，见 docs/switch/pitfalls.md"
    exit $rc
  fi
else
  echo "--- 步骤 4/4：跳过编译（加 --build 可自动验证）---"
fi

echo
echo "=== 同步完成 ==="
