# 更新记录

## 2026-11-06 · v1.20.4.4（退出崩溃修复）

- **核心修复：退出 PPSSPP 不再弹「由于发生错误，软件已关闭」**（真机已验证通过，用户 2026-11-06 确认）
  - 根因：本项目交付 `.nro`，`envIsNso()==false`，而 `SDL/SDLMain.cpp` 强制声明 `AppletType_Application`；libnx `_appletCleanup` 默认只在 `envIsNso()==true` 或 `__nx_applet_exit_mode==1` 时注册 `_appletExitProcess`（向 AM 发 `ISelfController cmd 0 Exit` 的退出握手）。默认 mode=0 让整个握手被跳过，HOS 判异常终止 → 弹框。
  - 修法：定义强符号 `u32 __nx_applet_exit_mode = 1;` 覆盖 libnx 的 weak 默认值，NRO 下也建立握手。
  - 同时修正退出 teardown：跳过 `fsdevUnmountAll()` 与 `fsExit()`（前者在有打开的 sdmc 日志 `FILE*` 时崩溃，后者 `sessionmgrClose` 会关掉日志写路径依赖的 fs 会话）；保留 `timeExit/hidExit/appletExit/smExit`，`__nx_exit(0, envGetExitFuncPtr())` 就地求值；日志全程可写并在结束前显式 `fclose` 落盘。
  - 增加运行时诊断：`envIsNso()/appletGetAppletType()/__nx_applet_type`、AM 握手 `retBefore/retAfter` 与 `handshake established/MISSING` 打印。
  - 完整七轮排障过程见 `docs/switch/退出崩溃修复日志.md`。
- **核心修复：怪物猎人 3 HD 装固件时游戏内中文缺字（显示下划线 `_`）已修复**（真机已验证通过）
  - 现象：安装 PSP 固件后，怪物猎人携带版 3rd 系列（含 MHP3rd 汉化版）游戏内道具名等**部分简体汉字变成下划线 `_`**；卸载固件后反而正常。固件又不能卸——XMB / UMD Video 功能依赖它，用户要求两者兼容。
  - 根因：游戏内文字走 PSP 的 `sceFont` / PGF 字体链路，**与 PPSSPP UI 的字体图集无关**。装完整固件后 `NandFontsComplete()` 判定成立 → 游戏运行盘内真实 `libfont.prx`（官方日文字库，只覆盖 JIS 汉字子集）→ 汉化版简体字查不到字形 → 以 `altCharCode=0x5F` 画出下划线。无固件时走 PPSSPP 的 HLE 字体（内置 `assets/flash0/font/jpn0.pgf`，4.3MB 扩展字库，覆盖简体中文），所以反而正常。
  - 修法：新增**按游戏生效的兼容开关 `ForceInternalFont`**（commit `619d789ccc`），只对目标游戏强制走内置扩展字库，不动全局字体：
    - `Core/Compatibility.h` / `Core/Compatibility.cpp`：新增 `ForceInternalFont` compat flag
    - `Core/HLE/sceFont.cpp`：开启时跳过游戏盘字体安装（`InstallFontsFromDiscUpdater()`）与 NAND 固件字体分支，直接读内置 VFS 扩展字库
    - `Core/HLE/HLE.cpp`：同步清掉 sceFont 的 Disable-HLE 位，保证字体走 HLE 实现
    - `assets/compat.ini`：为 5 个 discID 开启：`NPJH90146` / `ULJM05800` / `NPJB40001` / `NPJH55800` / `ULJM08058`
  - 效果：固件完整保留（XMB / UMD Video 不受影响），目标游戏内中文完整显示，用户真机确认字体正常。
  - 说明：该开关只对名单内游戏生效，其他游戏行为完全不变；今后其他游戏若遇同类缺字，按游戏 ID 在 `compat.ini` 里加同名开关即可。
- **本版包含此前已发布的功能**（简要列，不展开）：
  - 主线合并 `3bc60270e`（123 个提交）
  - XMB 系统更新双入口（首页按钮 + VSH 拦截，从 GitHub 检查更新）
  - UMD 插拔 / UMD Video、金手指汉化、`disc0:` I/O 埋点
  - Saved folders 配置写入修复、快捷方式 NRO 配置读取修复
- **产物**：`PPSSPP.nro` 74,004,918 字节，MD5 `7dffc2142ae91a1f21799657c9f59afe`，内嵌 release tag `v1.20.4.4`（在线更新器据此判新旧）
- 内部版本号跟随 PPSSPP 主线 1.20.4，release tag 顺延 `v1.20.4.x`。

## 2026-10-31 · 整理为可发布的开源项目

- 整理项目结构，补齐发布所需文档：
  - `README.md`（项目说明、血统、改动、状态、署名、协议）
  - `AI-REPLICATE.md`（给 AI 工具的一键复刻手册）
  - `docs/编译操作手册.md`（给人类的详细无坑操作手册）
  - `docs/已知问题.md`、`docs/switch/pitfalls.md`、`docs/switch/our-changes.md`
  - `NOTICE.md`（署名与第三方协议声明）、`LICENSE`、`.gitignore`
- 收录依赖构建脚本：`scripts/switch/deps/`（Mesa SDK / FFmpeg 57 / SDL3 补丁 / 子模块）
- 编译与同步脚本：`scripts/switch/build.sh`、`scripts/switch/sync-upstream.sh`

## 2026-10-30 · 合并上游主线 + UMD Video 真机验证通过（重要里程碑）

- **整仓合并上游主线 tip `61865a5c`**，一次性吃进 **2361 个上游提交**，冲突 39 个文件
  - 由此获得上游已实现的 **UMD Video 视频盘播放**能力
  - 解码主链路 `sceVideocodec.cpp` / `sceMpegbase.cpp` / `sceMp4.cpp` 与上游逐字节一致
- 修复 12 项 Switch 交叉编译适配问题（`HTTPS_NOT_AVAILABLE`、`aemu_postoffice_server` 去重、
  缺失 include、平台宏、库链接等）
- **真机验证通过**：
  - ✅ UMD Video **完美播放**
  - ✅ PSP 游戏 ISO（XMB 下挂载并运行）正常
  - ✅ HOME 键返回 XMB 正常
  - ✅ **帧率正常**（此前"XMB 启动后开局卡顿约 1 分钟"的问题一并消失）
  - ⚠️ 退出时仍会弹「由于发生错误，软件已关闭」（次要，重进即可）
- 产物：`PPSSPP.nro` 63,848,790 字节，md5 `9ba77e14613eb1e8147ef7d1dc60d294`

## 2026-10-30 · XMB 启动游戏的宿主侧身份同步

- `Core/HLE/sceKernelModule.cpp` 新增 `AdoptDiscTitleForHost()` / `RestoreShellTitleForHost()`
  - 从 XMB 的 UMD 槽启动游戏走 `sceKernelLoadExec`，**不经过 `CPU_Init`**，
    宿主侧的 disc ID / 兼容配置 / 内存布局 / 着色器缓存路径必须手动补齐
- 修 `assets/compat.ini` 中 `GoWFramerateHack60` 未生效的问题

## 2026-10-28 · Vulkan 方案编译成功

- 基于 NaGaa95 的 `ppsspp-nx`（Vulkan/NVK）编译通过
- 自建 Mesa Switch SDK（Mesa 26.2.2，commit `67e632f`）
- OpenGL ES 方案废弃（真机跑不了游戏、触屏错乱）

## 早期 · 基础能力

- XMB 内 UMD 热插拔（本项目招牌功能）
- FFmpeg 切换到自编的 57.24.102（解决 PMF 花屏 + 播完卡死）
- SDL3 Switch 驱动补丁：Vulkan 外部上下文 + **触屏修复**
  （`SDL_SendTouch` 第 5 参是事件类型不是 bool，原代码导致点击永不触发）
- Switch 平台适配与诊断埋点（`[umd]` `[umdio]` `[hletrace]` `[mem]` `[exit]` `[cache]`）

---

> 本项目修改者：**龙魂猪猪**
> 上游：`hrydgard/ppsspp`、`NaGaa95/ppsspp-nx`、`danfromtico/mesa-switch` / `NaGaa95/mesa-switch`
