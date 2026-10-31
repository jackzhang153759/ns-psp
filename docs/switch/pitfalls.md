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

### A4. `assets/font_atlas.zim/.meta` 不要手工替换 ⭐（2026-10-31 已更新）
**现状**：Switch 版**已启用 SDL3_ttf**（`USE_SDL3_TTF`）+ `assets/fonts/NotoSansSC-Regular.otf` 回退，
UI 中文不再依赖图集缺字。图集（UBUNTU24）仍是主字体，缺字时走 `CheckMissingGlyph` → Noto 回退。
**仍然不要**手工用 atlastool 重生成图集：换成覆盖两万字的字体后图集高度会超
`AtlasGen.cpp:118` 的 `maxHeight = width*2` 假设 → 真机 Vulkan 初始化成功后**立刻崩溃**。
且 `'z'` locale 的字符过滤在实际渲染时**未生效**（原因未查明），不要重试图集扩充路线——用 TTF 回退，已解决。
教训细节见下面 F9。原图集备份：`F:/SWITCH-psp/backup-font-atlas/font_atlas.{zim,meta}.orig`。

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

## F. 2026-10-31 会话新增（UI / 数据文件 / 打包）

### F1. 行尾规则：ini 保持 CRLF，金手指必须 LF；MSYS grep 会骗人 ⭐
- **`assets/lang/zh_CN.ini` 整个文件本来就是 CRLF**（1808 行全 `\r\n`）。之前 `grep -c $'\r'` 返回 0 是
  **MSYS/Git Bash 文本模式剥 CR 的假象** → 误判成"应该 LF"。
- **修改 ini 时保持文件原有 CRLF 风格**，别按"必须 LF"去"修"，会制造全文件 diff。
- **金手指 `cheat.db` 相反：必须 LF** —— PPSSPP 的 CWCheat 解析器读不了 CRLF，
  Windows 文本模式写文件（Python `"w"`）会写坏它。合并/生成脚本一律用 **`'rb'/'wb'` 二进制模式**，
  显式 `replace(b'\r\n', b'\n')`，并用 **Python rb 逐字节校验**（grep/diff 在 MSYS 层都会掩盖问题）。
- 教训：**"文本文件行尾"没有统一答案，以解析器实际行为 + 文件既有风格为准，且必须用二进制方式校验。**

### F2. 源码字符串编码混写 → TTF 渲染成方块字
UI Hint 曾写成 `\u4e0b` 转义 + 原始 UTF-8 字节**混写**，取决于编译器执行字符集，
产物里可能不是合法 UTF-8 → TextDrawerSDL 解码失败逐字节画方块。
**中文一律直接写 UTF-8 字面量**，编译后用 `strings`/二进制搜索验证 NRO 内该串恰出现 1 次。

### F3. fopencookie 两坑（日志时间戳流）
- `#define _GNU_SOURCE 1` 必须在**所有 include 之前**；
- PPSSPP `#define u64 _u64` 与 libnx 冲突 → cookie 回调里用 `uint64_t` 不要用 `u64`。

### F4. `atlastool_bin` 会残留在仓库根目录
编译失败时 `test -f` 仍通过 → 可能跑到**旧的**二进制得出错误结论。跑图集工具前先 `rm -f`。
（该文件已进 .gitignore，勿提交。）

### F5. nacptool 写中文作者会 GBK 乱码
nacptool 按 ANSI 处理非 ASCII。用 `scripts/switch/patch_nacp_author.py` 构建后把作者字段改回 UTF-8。

### F6. NSP forwarder 图标必须 JPEG
NACP 图标塞 PNG 是非法文件，桌面会显示问号。`tools/make_forwarder_nsp.py` 的
`write_icon_jpeg()` 会把任意输入归一化成 256×256 JPEG。

### F7. MSYS 路径转换会毁掉 sdmc 参数
`--sdmc /switch/...` 被 MSYS 转成 Windows 路径 → **加 `MSYS_NO_PATHCONV=1`**；
`--args --vsh` 会被 argparse 当成选项 → 写成 **`--args=--vsh`**。
生成器脚本里有防呆校验，但命令行直接调时自己注意。

### F8. APM 性能档：0x92220009/0x0A 会被拒
`apmSetPerformanceConfiguration` 传 boost 档 `0x92220009`/`0x9222000A` 真机返回 `rc=0x494`。
候选列表必须包含 **`0x00020000`（1785MHz）**，逐个尝试取第一个成功的
（见 `SDL/SDLMain.cpp` `SwitchRequestMaxPerformance()`）。

### F9. 字体图集扩充实验（❌ 失败，已回滚，不要重试）
- 请求集 ~9500 汉字 vs 原字体只覆盖 2397 → 换微软雅黑后图集涨到 2048×4146，
  超 `AtlasGen.cpp:118` 假设 → 真机**启动即崩**（Vulkan 初始化成功之后崩，极具迷惑性）；
- 新加 `'z'` locale + `charset_cn.txt`：`DEBUG` 显示集合正确（2599 字）但实际仍渲染 8767 字，
  **filter 未生效原因未查明**；宽度改 4096 更糟（4096×6242）；
- **正确路线 = SDL3_ttf + TTF 回退（已实现）**。原图集备份在 `F:/SWITCH-psp/backup-font-atlas/`。

### F10. git-version.cmake describe 拿错版本
本仓库有怪 tag `1.0.0` + shallow clone 看不到正式 tag →
`git describe` 要加 `--match "v1.*"`，克隆时 `--unshallow`。版本号由
`CMakeLists.txt` 的 `PPSSPP_SWITCH_RELEASE_VERSION "1.20.4"` FORCE 钉住，跟随主线手动更新。

---

## G. 快速诊断表

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
| UI 中文变问号/方块 | 字符串编码混写（F2）或动了图集（A4/F9） |
| 金手指读不出/解析错 | cheat.db 是 CRLF（F1）或含坏 `_L` 行 |
| 桌面快捷方式图标是问号 | NSP 图标不是 JPEG（F6） |
| 传给 nro 的参数路径变味 | MSYS 路径转换（F7） |
| apm boost 被拒 rc=0x494 | 候选列表缺 0x00020000（F8） |
| ini 一改就全文件 diff | 行尾风格判断错了（F1） |
| 版本号显示成 1.0.0 或怪版本 | describe 未加 `--match "v1.*"`（F10） |
