# 踩坑清单（Switch 移植）

按类别整理。**编译失败时先来这里查**。

---

## A. 环境与构建

### A1. cmake 找不到 → 必须用 devkitPro 的 msys2 bash
`cmake` 只装在 devkitPro 的 msys2 里。用 PortableGit / 系统 Git Bash 跑会 `command not found`。

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "bash scripts/switch/build.sh"
```

### A2. 构建目录必须在 C 盘 NTFS
放在 F 盘（exFAT）会导致 cmake 的 `try_compile` 失败。默认 `/c/switch-build/ppsspp-nx-build2`。

### A3. FFmpeg 必须是 57，绝不能用 7.x ⭐最容易翻车
- ✅ `hrydgard/ppsspp-ffmpeg @ 82049cca` 自编的 **libavcodec 57.24.102**（`C:/switch-build/ffmpeg57/prefix`）
- ❌ devkitPro 的 `switch-ffmpeg`（libavcodec 61 / FFmpeg 7.x）

用错的症状：**PMF 过场动画花屏/绿屏，且动画播完必定卡死**（画面停、无声音、进程不崩）。
没有 ffmpeg 则动画出不来 → 游戏等不到视频结束 → 黑屏。

`-DFFMPEG_DIR` 要传 msys2 风格 `/c/...`（`C:/...` 会被当相对路径）。
改 FFmpeg 版本后要 `-U "FFmpeg_*" -U "HAVE_LIBAVCODEC*"` 清缓存，否则残留 NOTFOUND。

### A4. 不要替换 `assets/font_atlas.zim/.meta`
预生成的字体图集，Switch 版不走 SDL3_ttf。手工用 atlastool 重生成会**启动即崩**。
UI 缺字的正确路线是启用 SDL3_ttf + 字体回退（需交叉编 SDL3_ttf + harfbuzz）。

### A5. 触屏只有高亮、点不动 → SDL3 没打补丁
`SDL_SendTouch` 第 5 个参数是**事件类型**不是 bool。原代码传 `true/false` = `type=0/1`，
内部 `down = (type == SDL_EVENT_FINGER_DOWN)` 恒为 false → 每次触摸都被当成"抬起"。
补丁：`scripts/switch/deps/sdl3-switch-vulkan-and-touch.patch`

### A6. `rm -rf` 被安全钩子拦
文件数超阈值。换新构建目录，或让用户确认。

---

## B. Mesa Switch SDK

### B1. 脚本 CRLF → 容器里 shebang 失效
`No such file or directory`。修：`find . -name "*.sh" -exec sed -i 's/\r$//' {} +`

### B2. 不要把 `/usr/local/libexec` 放 PATH 前面
ninja 的"宿主编译器"会命中交叉 rustc 包装 → proc-macro 被按 aarch64 编译 → 链接失败。

### B3. 不要用 mesa 的 `nouveau.h` 覆盖 portlibs 的
会顶掉 libdrm_nouveau 的 `struct nouveau_list` → gallium winsys 编不过。

### B4. `--depth 1` 拉子模块会拿默认分支最新版
不是 pin 的 commit。用 codeload tarball 按 SHA 下载（`scripts/switch/deps/fetch_submodules.sh`）。

### B5. devkitPro 精简安装缺 `libexpat.a` / `libzstd.a`
从容器 `docker cp` 到 `mesa-sdk/lib`。

### B6. Switch 平台需要空 `libelf.a` 桩
Vulkan/OpenGL 的 cmake 配置要求 libelf，但 Switch 平台 mesa 不需要 → 造个空的。

### B7. Mesa 的 `.a` 符号索引不完整 → 几千个 undefined reference
gcc-ar 的 LLVM14 插件读不了 LLVM23 目标文件。
**全部用 `aarch64-none-elf-ranlib` 重建索引**。

### B8. `-Doptimization` 必须是 2
设为 1 会启用 share-generics → Rust 泛型实例不落地 → 链接时几千个 undefined。

### B9. 构建参数
```
-Dplatforms=switch -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau,zink -Doptimization=2
```
`libvulkan.a` 是合并好的大包（含 nvk/nil/nak/nir + rust 运行时），ppsspp 只链它。

---

## C. 合并上游后的交叉编译适配（12 项已修，勿改回）

| # | 文件 | 修复 | 不修的症状 |
|---|---|---|---|
| 1 | `CMakeLists.txt` | USE_LIBNX 分支 `set(HTTPS_NOT_AVAILABLE ON)` | `undefined reference to http::HTTPSShutdown()` |
| 2 | `Core/CMakeLists.txt` | `aemu_postoffice_server` 包在 `if(NOT USE_LIBNX)` 内 | enet 重复符号（`multiple definition of enet_host_service`） |
| 3 | `Core/HLE/sceNetAdhoc.cpp` | relay 线程 + `aemu_postoffice_server_log()` 用 `#if !PPSSPP_PLATFORM(SWITCH)` 屏蔽 | 同上，且 Switch 无该依赖 |
| 4 | `Core/HLE/sceKernelModule.cpp` | 补 `#include "Core/HLE/sceKernelMemory.h"` | `kernelMemory` / `userMemory` / `PSP_SMEM_*` / `KERNEL_PARTITION_ID` 等几十个符号未声明 |
| 5 | `Core/HLE/scePower.cpp` | 删掉重复的 `PowerScaleFromDefaultClock()` | redefinition |
| 6 | `Core/HLE/sceUmd.cpp` | `sceUmdDeactivate` 里补 `const char *name` 声明 | `name was not declared` |
| 7 | `Core/Loaders.cpp` | `UmdMount` 里补 `IFileSystem *currentUMD` | `currentUMD was not declared` |
| 8 | `Core/MIPS/JitCommon/JitProfiler.cpp` | 加 `#elif PPSSPP_PLATFORM(SWITCH)` 分支 | `dlfcn.h: No such file or directory`（libnx 无 dlfcn.h / sys/ucontext.h / cxxabi.h / execinfo.h） |
| 9 | `SDL/SDLJoystick.cpp` | 补 `#include "Core/Config.h"` | `g_Config was not declared`（我们用 `g_Config.bRightStickFaceButtons`） |
| 10 | `Common/GPU/Vulkan/VulkanQueueRunner.cpp` | `CreateBackbufferFramebuffer` 返回 `VK_NULL_HANDLE` | `cannot convert 'bool' to 'VkFramebuffer'` |
| 11 | `ext/CMakeLists.txt` | aemu **server** 设 C++20 + `ENET_IPV4_ONLY` | `'counting_semaphore' is not a member of 'std'`；libnx 的 `in6_addr` 布局不同 |
| 12 | `Core/HLE/sceMpegbase.cpp` | `PerformWriteFormattedFromMemory` 用 **5 参**（传 `rangeWidth`） | 参数不匹配 |

⚠️ 第 11 条细节：libnx 的 `struct in6_addr` 联合体首成员是 `uint16_t __u6_addr16[8]`（glibc 是 `uint8_t[16]`），
所以 server 要 `ENET_IPV4_ONLY`；但 **client 不能开**（`connect_enet()` 要写 `enet_addr.host.s6_addr`）。

⚠️ 第 5 条那个重复定义，是历史上为了适配上游 `sceVideocodec`/`sceMpegbase` 手工加进去的，
合并后上游自己也带同名函数 → 撞名 → 删掉一份即可，**不要再补回**。

---

## D. git / 工具链陷阱

### D1. ⭐ 本仓库 `git status` 是坏的，会静默误报「0 改动」
`ext/aemu_postoffice` 子模块损坏（缺 `.git/modules/ext/aemu_postoffice`），
导致 `git status` / `git diff` 扫描时 fatal 退出、**什么都不报**。

→ 真实状态可能是"工作区有大量未提交改动"。
→ 查看真实改动必须用：
```bash
git diff --ignore-submodules=all
git show HEAD:<路径>          # 与工作树对比
```
**永远不要相信本仓库的裸 `git status`。**（曾因此误判"修复已提交"，实际全在工作树里未提交。）

### D2. 长编译不能在主理人/lead 自己的会话里跑
进程会在该轮 turn 结束时被环境杀掉，且**毫无报错**（日志就停在某百分比，如 52%、66%）。
`run_in_background` 撑不过 turn 结束；`setsid` + `nohup` 脱离反而死得更快（45 秒就被清）。

→ **正解：把构建派给子 agent（worker）**，worker 自己的会话能跑完整流程。
→ 也不要同时开两个构建写同一构建目录（会损坏 build 目录）。

### D3. 上游 tip 要取对引用
`ppsspp-upstream/` 的工作树可能停在旧 commit。取代码一律用 `origin/master` 引用
（`git show origin/master:<path>`）。
从本地镜像 remote 拉时要取 `refs/remotes/origin/master`，**直接取 `master` 会拿到旧提交**。

---

## E. 快速诊断表

| 症状 | 原因 |
|---|---|
| PMF 花屏 + 播完卡死 | FFmpeg 用成了 7.x（A3） |
| `cmake: command not found` | 没用 msys2 bash（A1） |
| cmake `try_compile` 失败 | 构建目录在 exFAT（A2） |
| 触屏点不动 | SDL3 未打补丁（A5） |
| 启动即崩 | 动了 `assets/font_atlas.*`（A4） |
| 几千个 `undefined reference` | Mesa `.a` 索引未重建（B7）或 optimization≠2（B8） |
| `http::HTTPSShutdown()` 未定义 | 缺 `HTTPS_NOT_AVAILABLE ON`（C1） |
| enet 重复符号 | Switch 链了 `aemu_postoffice_server`（C2/C3） |
| `git status` 报 0 改动但明明改了 | 子模块损坏（D1） |
| 编译"卡住"不动、无报错 | 进程被环境杀（D2）→ 交给 worker 跑 |
