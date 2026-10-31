# AI 复刻指南 · PPSSPP → Nintendo Switch 移植

> **这份文档是给 AI 编码助手 / 自动化工具看的。**
> 按本文档从上到下执行，即可从零复刻出一个能在 Switch 上运行的 PPSSPP（含 **UMD Video 播放**、
> **XMB 内 UMD 热插拔**、PSP 游戏 ISO 运行）。
> 每一步都给出：要执行的命令、**预期结果**、**失败时的处理办法**。
> 人类读者请改看 [`SWITCH-PORT.md`](SWITCH-PORT.md)。

---

## 0. 这个项目是什么

| 项 | 说明 |
|---|---|
| 上游 | [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp)（PSP 模拟器主线） |
| 移植基础 | [NaGaa95/ppsspp-nx](https://github.com/NaGaa95/ppsspp-nx)（Switch / libnx + Vulkan/NVK 移植） |
| 本仓库 | 在上两者之上，叠加了**我们自己的功能**并持续同步主线 |
| 产物 | `PPSSPP.nro`（约 64 MB），放到 SD 卡 `SD:/switch/ppsspp/PPSSPP.nro` |

**本仓库相对上游多出来的能力（这是移植的价值所在）**：
1. **XMB 内插入/弹出 UMD 光盘镜像**（原版只能从 PPSSPP 自己的菜单启动游戏，不能在 XMB 里换盘）
2. XMB 启动游戏时的**宿主侧身份同步**（disc ID / 兼容配置 / 内存布局 / 着色器缓存路径）
3. **UMD Video 视频盘播放**（2026-10-30 通过整仓合并上游主线后真机验证通过）
4. Switch 平台的各种适配与诊断埋点

---

## 1. 前置条件

| 依赖 | 要求 | 备注 |
|---|---|---|
| 操作系统 | Windows 10/11 | 全部脚本按 Windows + Git Bash / msys2 编写 |
| devkitPro | 含 `devkitA64`、`libnx`、`portlibs/switch` | 默认装在 `C:/devkitPro` |
| **cmake** | **只在 devkitPro 的 msys2 里有** | ⚠️ 所有构建命令必须用 `/c/devkitPro/msys2/usr/bin/bash.exe -lc "..."`，用 PortableGit 的 bash 会找不到 cmake |
| Docker Desktop | 用于构建 Mesa SDK | `C:/Program Files/Docker/Docker/resources/bin/docker.exe`（不在 PATH，需手动加入） |
| 磁盘 | C 盘 ≥ 30 GB | ⚠️ 构建目录**必须在 C 盘 NTFS**；放 F 盘（exFAT）会让 cmake 的 `try_compile` 失败 |
| 网络 | 首次需联网 | 拉上游、子模块、Docker 镜像 |

---

## 2. 目录约定

```
<REPO>/                      本仓库（ppsspp-nx）
├── AI-REPLICATE.md          本文件（AI 入口）
├── SWITCH-PORT.md           人类读的移植说明
├── scripts/switch/
│   ├── build.sh             编译 NRO（主脚本）
│   ├── sync-upstream.sh     同步主线 + 合并 + 编译验证
│   └── deps/                依赖构建脚本（Mesa / FFmpeg / SDL3 patch / 子模块）
└── docs/switch/
    ├── UPSTREAM-BASE        已合并的上游 commit（sync 脚本用）
    ├── pitfalls.md          ⚠️ 踩坑清单（编译失败先查这里）
    └── our-changes.md       我们相对上游的改动清单（合并冲突时必读）
```

外部（不随仓库发布）的构建产物前缀，可用环境变量覆盖：

| 变量 | 默认 | 说明 |
|---|---|---|
| `BUILD_DIR` | `/c/switch-build/ppsspp-nx-build2` | 构建目录（**必须 C 盘**） |
| `MESA_SDK` | `/c/switch-build/mesa-sdk` | Mesa Switch SDK |
| `FFMPEG_SDK` | `/c/switch-build/ffmpeg57/prefix` | **FFmpeg 57**（见 §3.3） |
| `SDL_PREFIX` | `/c/switch-build/sdl-prefix` | 打过补丁的 SDL3 |

---

## 3. 环境搭建（首次，约 1 小时）

**全部命令都要在 devkitPro 的 msys2 bash 里执行：**

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "echo ok"
```
> 预期：输出 `ok`。若提示找不到 bash → devkitPro 没装或路径不同，先修正路径。

### 3.1 Mesa Switch SDK（Vulkan/NVK 驱动）

Switch 的 Vulkan 来自 Mesa 的 nouveau/NVK 驱动，需要交叉编译成静态库 SDK。

```bash
# 构建 Docker 镜像（约 5 分钟）
export PATH="/c/Program Files/Docker/Docker/resources/bin:$PATH"
docker build -f C:/switch-build/mesa-ctx/Docker.rust -t devkitpro-mesa-rust C:/switch-build/mesa-ctx

# 容器内构建（约 10 分钟）
MSYS_NO_PATHCONV=1 docker exec mesa-build bash -c "WIPE=1 bash /project/build-unified-docker.sh"
```

产品落在 `C:/switch-build/mesa-out/stage/opt/devkitpro/portlibs/switch`，拷到 `C:/switch-build/mesa-sdk`，
并**补齐 `libexpat.a` / `libzstd.a` / `libz.a` / 空 `libelf.a` 桩**（devkitPro 精简安装缺这些，
详见 [`docs/switch/pitfalls.md`](docs/switch/pitfalls.md) 第 5–7 条）。

> 更省事的做法：直接用 `scripts/switch/deps/build-mesa-sdk-docker.sh`。

### 3.2 SDL3（**必须打我们的补丁**）

Switch 的 SDL3 驱动需要两处修改，否则触屏点击失灵、Vulkan 上下文创建失败：
- Vulkan 外部上下文（不调 EGL）
- **触摸事件的第 5 个参数是事件类型，不是 bool**（原代码传 `true/false` → 被当成 `type=0/1`，
  内部 `down` 恒为 false → UI 只有高亮、永远不触发点击）

```bash
# SDL3 源码放到 F:/SWITCH-psp/sdl-src 后打补丁
cd /f/SWITCH-psp/sdl-src
git apply /f/SWITCH-psp/ppsspp-nx/scripts/switch/deps/sdl3-switch-vulkan-and-touch.patch
# 然后交叉编译安装到 C:/switch-build/sdl-prefix
```

### 3.3 FFmpeg —— ⚠️ **必须是 57，不是 7.x**

这是本项目**最容易翻车的一条**。

- ✅ 用 `hrydgard/ppsspp-ffmpeg @ 82049cca2e4c1516ed00a77b502a21f91b7843f4` 自己编的 **libavcodec 57.24.102**
- ❌ **绝不能**用 devkitPro 的 `switch-ffmpeg`（libavcodec 61 / FFmpeg 7.x）

用错版本的症状（很难排查，直接记住结论）：
> PMF 过场动画**花屏/绿屏**，并且**动画播完必定卡死**（画面停住、无声音、进程不崩）。
> 没有 ffmpeg 则动画根本出不来 → 游戏等不到视频结束 → 黑屏。

```bash
bash scripts/switch/deps/build-switch-ffmpeg57.sh     # 约 5 分钟
```
> 产物前缀：`C:/switch-build/ffmpeg57/prefix`
> 验证：`grep LIBAVCODEC_VERSION_MAJOR $FFMPEG_SDK/include/libavcodec/version*.h` 应为 **57**。

---

## 4. 编译 NRO

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "bash /f/SWITCH-psp/ppsspp-nx/scripts/switch/build.sh"
```

> **预期结果**：
> - 末尾输出 `=== 编译完成 ===` 并列出行 `PPSSPP.nro`
> - `$BUILD_DIR/PPSSPP.nro` 存在，约 **63–64 MB**
> - 日志里 `error:` 计数为 **0**

> **失败怎么办**：先查 [`docs/switch/pitfalls.md`](docs/switch/pitfalls.md)。
> 编译错误几乎都是"交叉编译适配"问题（缺 include / 缺声明 / 平台宏 / 库链接），
> **不是**代码没合进来——本项目已审计确认上游实现完整（见 §7）。

---

## 5. 部署与验收

```bash
cp $BUILD_DIR/PPSSPP.nro /f/SWITCH-psp/output/PPSSPP.nro     # 或用资源管理器拷
```
拷到 SD 卡 `SD:/switch/ppsspp/PPSSPP.nro`。**XMB 需要另放 6.60 固件文件。**

真机验收清单（本项目已于 2026-10-30 全部通过）：

| 项 | 预期 |
|---|---|
| 进 XMB | 正常显示 PSP 界面 |
| **插入 UMD Video ISO 并播放** | ✅ 能播放 |
| XMB 插入游戏 ISO 并运行 | ✅ 正常运行 |
| HOME 键返回 XMB | ✅ 正常 |
| 游戏帧率 | ✅ 正常 |
| 退出 | ⚠️ 已知会弹「由于发生错误，软件已关闭」（次要，重进即可，未修） |

日志：NRO 把 stdout/stderr 写到 SD 卡根目录 **`sdmc:/ppsspp_log.txt`**。
排错时重点看这几组标记：`[umd]`（插盘/盘类型）、`[umdio]`（devctl）、`[hletrace]`（guest 调用）、
`[mem]`（JIT 内存映射，与退出崩溃相关）、`[exit]`（退出流程）、`[cache]`（着色器缓存）。

---

## 6. 同步上游主线（每次运行自动拉取 + 验证）

```bash
bash scripts/switch/sync-upstream.sh --check          # 只检查：落后主线多少提交（默认，最安全）
bash scripts/switch/sync-upstream.sh --merge           # 拉取并合并到当前分支
bash scripts/switch/sync-upstream.sh --merge --build   # 合并后自动编译验证
```

脚本行为：
1. 从 `hrydgard/ppsspp` 拉取 `master` 到本地分支 `upstream-master`
2. 与 `docs/switch/UPSTREAM-BASE` 里记录的"上次已合并的上游 commit"比对，报告落后多少提交
3. `--merge` 时执行 `git merge --no-ff --no-commit`；**有冲突会停下来并列出冲突文件**（不强行解决）
4. 合并成功后自动更新 `UPSTREAM-BASE`；`--build` 会接着跑一次编译验证

> ⚠️ **本仓库的 `git status` 是坏的**：`ext/aemu_postoffice` 子模块损坏（缺 `.git/modules/...`），
> 会导致 `git status` / `git diff` 扫描时 fatal 退出、**静默报「0 改动」**。
> 想看真实改动必须用 `git diff --ignore-submodules=all`，或用 `git show HEAD:<路径>` 与工作树对比。
> **永远不要相信本仓库的裸 `git status`。**

---

## 7. 合并冲突时：什么必须保住

合并上游时若冲突，**我们的功能优先级高于上游**（除非上游自己实现了同样的事）。
完整清单见 [`docs/switch/our-changes.md`](docs/switch/our-changes.md)，核心红线：

| 文件 | 必须保住的内容 |
|---|---|
| `Core/HLE/sceUmd.cpp` / `.h` | `__UmdInsert` 宿主记账、`__UmdNoDiscAtBoot` 的 `disc0:` 提前返回、两段式插入通知 |
| `Core/Loaders.cpp` | `UmdReplace()`；`UmdMount` 里的 `currentUMD` 声明 |
| `Core/PSPLoaders.cpp` | `LoadParamSFOFromDisc()` |
| `Core/HLE/sceKernelModule.cpp` | `AdoptDiscTitleForHost()` / `RestoreShellTitleForHost()`（XMB 启动游戏的宿主身份同步） |
| `Core/HLE/sceIo.cpp` | devctl `0x01F20001` 返回真实盘类型 |
| `GPU/GPUCommon.h`、`GPU/Vulkan/GPU_Vulkan.cpp` | `ReloadShaderCache()` |
| `Core/HLE/sceDisplay.cpp` | `__DisplayResetFrameTiming()` |
| `UI/PauseScreen.cpp` + `assets/lang/*.ini` | Insert / Eject UMD 菜单项与文案 |
| `Core/HW/MediaEngine.cpp` | Switch 解码线程数 = 1 |
| `SDL/SDLMain.cpp` | 日志追加模式、`__nx_win_exit` 防重入 |

规则：**编译报错时只做最小修补**（补 include / 补声明 / 加平台 guard），
**绝不删除我们的逻辑或改回上游写法**。

---

## 8. 已知坑的极简速查

| 症状 | 原因 |
|---|---|
| PMF 过场动画花屏 + 播完卡死 | FFmpeg 用成了 7.x，必须是 57 |
| `cmake: command not found` | 没用 devkitPro 的 msys2 bash |
| cmake `try_compile` 失败 | 构建目录在 exFAT（F 盘），要放 C 盘 NTFS |
| 触屏只有高亮、点不动 | SDL3 没打我们的补丁（触摸事件类型参数） |
| 几千个 `undefined reference` | Mesa 的 `.a` 符号索引不完整，要 `aarch64-none-elf-ranlib` 重建 |
| 链接报 `http::HTTPSShutdown()` 未定义 | Switch 分支要 `set(HTTPS_NOT_AVAILABLE ON)` |
| enet 重复符号 | Switch 不要链 `aemu_postoffice_server`（用 `if(NOT USE_LIBNX)` 包住） |
| `git status` 显示 0 改动但明明改了 | 子模块损坏导致静默失效，用 `--ignore-submodules=all` |

完整版见 [`docs/switch/pitfalls.md`](docs/switch/pitfalls.md)。
