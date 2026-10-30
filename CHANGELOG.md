# 更新记录

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
