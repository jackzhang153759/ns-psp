# AI 复刻手册 · NSpsp（PPSSPP for Nintendo Switch）

> ## 给 AI 助手的指令（请把本文件完整读完再动手）
>
> 你是一个 AI 编码助手。用户给了你这个仓库，希望你**从零复现出可运行的 `PPSSPP.nro`**。
> 请**严格按本文件从上到下执行**，不要跳步、不要自作主张改构建参数。
> 每一步都写明了：**要执行的命令** → **预期结果** → **失败怎么办**。
> 遇到失败，先查第 8 节速查表和 `docs/switch/pitfalls.md`，再重试。
> 本仓库是 GPL-2.0-or-later 项目的衍生作品，修改者：**龙魂猪猪**。
> 更多背景见 [`README.md`](README.md)（项目说明、血统、改动清单）与
> [`docs/编译操作手册.md`](docs/编译操作手册.md)（人类版详细手册，含同样的内容与更多细节）。

---

## 1. 目标与验收

| 项 | 内容 |
|---|---|
| 目标 | 编译出 Switch 自制固件可运行的 `PPSSPP.nro`（Vulkan / Mesa NVK） |
| 关键能力 | XMB 界面、**XMB 内 UMD 热插拔**、**UMD Video 视频盘播放**、PSP 游戏 ISO 运行 |
| 构建成功判据 | ① 末尾输出 `=== 编译完成 ===`；② `PPSSPP.nro` 约 **63–64 MB**；③ 日志 `error:` 计数 **0** |
| 真机验收 | UMD Video 能播 / PSP ISO 能跑 / HOME 返 XMB / 帧率正常 |

## 2. 前置条件

- Windows 10/11；devkitPro（含 devkitA64、libnx、portlibs/switch），默认 `C:/devkitPro`
- **cmake 只装在 devkitPro 的 msys2 里** → 所有构建命令必须用
  `/c/devkitPro/msys2/usr/bin/bash.exe -lc "..."`
- Docker Desktop（构建 Mesa SDK 用），`C:/Program Files/Docker/Docker/resources/bin/docker.exe`
- C 盘 ≥ 30 GB

先验证：
```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "echo ok"
```
> 预期 `ok`。

## 3. 三条铁律（违反必失败）

1. **FFmpeg 必须是 57（libavcodec 57.24.102）**，不能用 devkitPro 的 `switch-ffmpeg`（7.x）。
   用错 → PMF 过场动画花屏且**播完必定卡死**。
2. **构建目录必须在 C 盘 NTFS**，放 exFAT 盘会让 cmake `try_compile` 失败。
3. **不要重新生成 `assets/font_atlas.zim/.meta`**（会导致启动即崩）；
   **不要同时开两个构建写同一构建目录**。

## 4. 依赖准备（首次约 1–2 小时）

### 4.1 Mesa Switch SDK（Vulkan/NVK）

源码 `https://github.com/NaGaa95/mesa-switch.git`，本项目锁定 **`67e632f`**（Mesa **26.2.2**）。

```bash
export PATH="/c/Program Files/Docker/Docker/resources/bin:$PATH"
docker build -f C:/switch-build/mesa-ctx/Docker.rust -t devkitpro-mesa-rust C:/switch-build/mesa-ctx
MSYS_NO_PATHCONV=1 docker exec mesa-build bash -c "WIPE=1 bash /project/build-unified-docker.sh"
```

或用仓库脚本 `scripts/switch/deps/build-mesa-sdk-docker.sh`。

**构建参数必须**：`-Dplatforms=switch -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau,zink -Doptimization=2`
（`optimization` 必须是 2，否则 Rust 泛型实例不落地 → 几千个 undefined）

**收尾三步（必做）**：产物拷到 `C:/switch-build/mesa-sdk`
→ 补 `libexpat.a` / `libzstd.a` / `libz.a` 与空 `libelf.a` 桩
→ **对所有 `.a` 跑 `aarch64-none-elf-ranlib` 重建索引**（否则几千个 `undefined reference`）。

### 4.2 SDL3（必须打补丁）

```bash
cd /path/to/sdl-src
git apply <仓库>/scripts/switch/deps/sdl3-switch-vulkan-and-touch.patch
# 交叉编译安装到 C:/switch-build/sdl-prefix
```

补丁作用：Vulkan 外部上下文 + **触屏修复**（`SDL_SendTouch` 第 5 个参数是事件类型不是 bool）。
不打补丁 → 触屏只有高亮、点不动。

### 4.3 FFmpeg 57

```bash
bash scripts/switch/deps/build-switch-ffmpeg57.sh
```

验证：
```bash
grep LIBAVCODEC_VERSION_MAJOR C:/switch-build/ffmpeg57/prefix/include/libavcodec/version*.h
```
> 预期 `57`。若是 `61` → 错了，重编。

### 4.4 子模块

```bash
bash scripts/switch/deps/fetch_submodules.sh      # 不要用 --depth 1
```

## 5. 编译

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "bash <仓库>/scripts/switch/build.sh"
```

可用环境变量覆盖路径：`BUILD_DIR`（默认 `/c/switch-build/ppsspp-nx-build2`）、
`MESA_SDK`、`FFMPEG_SDK`、`SDL_PREFIX`、`JOBS`。

> 若编译**看起来卡住且日志无报错** → 是进程被外部杀掉，非编译错误。
> 重新执行同一命令即可**增量续编**（不要 clean、不要换目录）。

## 6. 部署与验收

拷到 SD 卡 `SD:/switch/ppsspp/PPSSPP.nro`；**XMB 需自备 6.60 固件**（仓库不含）。
日志在 `sdmc:/ppsspp_log.txt`，排错看 `[umd]` `[umdio]` `[hletrace]` `[mem]` `[exit]` `[cache]`。

## 7. 同步上游主线

```bash
bash scripts/switch/sync-upstream.sh --check          # 检查落后多少提交
bash scripts/switch/sync-upstream.sh --merge           # 拉取并合并
bash scripts/switch/sync-upstream.sh --merge --build   # 合并后自动编译验证
```

冲突时**必须保住我们的改动**，红线清单见 `docs/switch/our-changes.md`。

## 8. 失败速查

| 症状 | 原因 |
|---|---|
| PMF 花屏 + 播完卡死 | FFmpeg 用成了 7.x（应为 57） |
| `cmake: command not found` | 没用 msys2 bash |
| cmake `try_compile` 失败 | 构建目录在 exFAT |
| 几千个 `undefined reference` | Mesa `.a` 未 ranlib 重建 / optimization≠2 |
| 触屏点不动 | SDL3 未打补丁 |
| 启动即崩 | 动了 `assets/font_atlas.*` |
| `http::HTTPSShutdown()` 未定义 | 缺 `HTTPS_NOT_AVAILABLE ON` |
| enet 重复符号 | Switch 链了 `aemu_postoffice_server` |
| `git status` 报 0 改动但明明改了 | 子模块损坏，用 `git diff --ignore-submodules=all` |

完整清单：`docs/switch/pitfalls.md`。

## 9. 不要做的事

- 不要改 `assets/font_atlas.zim/.meta`
- 不要把 FFmpeg 换成 devkitPro 的 7.x
- 不要把构建目录放到非 C 盘 NTFS
- 不要在合并上游时删除我们的 XMB / UMD 逻辑（红线见 `docs/switch/our-changes.md`）
- 不要往仓库里放 PSP 固件、游戏 ISO、UMD 镜像（版权内容，仓库不含这些）
