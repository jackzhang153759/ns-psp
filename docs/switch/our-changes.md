# 我们相对上游的改动（合并主线时的红线清单）

合并 `hrydgard/ppsspp` 主线时若发生冲突，**以下内容优先保住**。
规则：编译报错只做**最小修补**（补 include / 补声明 / 加平台 guard），
**绝不删除我们的逻辑或改回上游写法**。

完整 patch（历史快照，19 个文件）：`scripts/switch/deps/ppsspp-nx-vsh-umd-insert.patch`

---

## 一、核心功能改动

### 1. XMB 内 UMD 热插拔（本移植的招牌功能）

| 文件 | 内容 |
|---|---|
| `Core/HLE/sceUmd.cpp` | `__UmdInsert()`（宿主记账：`umdImagePath`、`__UmdSetDiscType`、`HleTraceNextCalls`）、`__UmdReplace()`、`__UmdNoDiscAtBoot()`、`__UmdEject()`、两段式插入通知 |
| `Core/HLE/sceUmd.h` | UMD 状态接口（`__UmdDiscType()` 返回 **u32 位掩码**，空驱返回 0） |
| `Core/Loaders.cpp` | `UmdReplace()`（卸载旧盘 + 挂载新盘）；`UmdMount()` 里的 `currentUMD` 声明 |
| `Core/PSPLoaders.cpp` | `LoadParamSFOFromDisc()` |
| `Core/HLE/sceIo.cpp` | devctl `0x01F20001` 返回真实盘类型（`__UmdDiscType()`）+ `[umdio]` 埋点 |
| `UI/PauseScreen.cpp` | Insert / Eject UMD 菜单项 |
| `assets/lang/*.ini` | 菜单文案（`zh_CN.ini` 里**故意保留英文**，因为对应汉字不在字体图集里） |

要点：
- `UMDInserted` 全局默认 `true`（为直接启动游戏设计）；进 VSH 时必须 `__UmdNoDiscAtBoot()` 清掉，
  否则暂停菜单直接显示"弹出 UMD"
- `__UmdEject` 里 `UnscheduleEvent` 的 userdata 必须与调度时一致
- 两段式通知共用 `umdInsertChangeEvent`：上游用 userdata 1、`__UmdReplace` 用 0，**取消时两个都要清**
- `__UmdNoDiscAtBoot()` 的 `disc0:` 提前返回是**有意的修复**（`--mount` 与"退出回 XMB"走
  `Load_PSP_ELF_PBP` 直接挂盘、从不调 `__Umd*`，不加这个保护会让视频盘被误判成空驱）

详见 [`vsh-umd-insert.md`](vsh-umd-insert.md)。

### 2. XMB 启动游戏的宿主侧身份同步

从 XMB 的 UMD 槽启动游戏走的是被模拟 PSP 内部的 `sceKernelLoadExec`，**不经过 `CPU_Init`**，
所以宿主侧初始化必须自己补齐。

`Core/HLE/sceKernelModule.cpp`：
- `AdoptDiscTitleForHost()`：`compat.Load(discID)` + `g_Config.LoadGameConfig(discID)`
  + `InitMemorySizeForGame()` + `PSP_CoreParameter().fileToStart/fileType`
  + `gpu->ReloadShaderCache(discID)`，并同步 `UI/EmuScreen.cpp` 的 `gamePath_`
- `RestoreShellTitleForHost()`：退回 XMB 时反向还原 + `compat.Reset()` + `g_Config.UnloadGameConfig()`

三条铁律：
- `g_runningVSH` **不能**用来判断"谁在前台"（`__KernelLoadReset()` 每次 LoadExec 都会清零它）
- adopt 必须用「`fileToStart` 不是 vshmain 就早退」来保住"主菜单直接启动"的路径
- `RequireBufferedRendering` 只是弹 OSD 警告，**不是渲染开关**

### 3. 着色器缓存 / 性能

| 文件 | 内容 |
|---|---|
| `GPU/GPUCommon.h`、`GPU/Vulkan/GPU_Vulkan.cpp` | `ReloadShaderCache(discID)` |
| `GPU/Vulkan/PipelineManagerVulkan.cpp` | 缓存加载 / 保存 + 计数诊断 |
| `Core/HLE/sceDisplay.cpp` | `DoFrameTiming` 慢帧诊断 + `__DisplayResetFrameTiming()` |
| `Core/System.cpp` | `[perf]` 诊断快照 + `ReloadShaderCache` 调用 |

> 注：历史上"XMB 启动后开局卡顿约 1 分钟"的问题，在 2026-10-30 大合并后**已自然消失**（用户真机确认帧率正常），
> 不必再排查着色器缓存容器。

### 4. Switch 平台适配

| 文件 | 内容 |
|---|---|
| `Core/HW/MediaEngine.cpp` | Switch 解码线程数 = 1 |
| `Common/MemArenaHorizon.cpp` | JIT 内存 view 诊断 |
| `SDL/SDLMain.cpp` | 日志追加模式（512KB 上限）、`__nx_win_exit` 防重入、`__libnx_exception_handler` |
| `UI/NativeApp.cpp` | 清 failed-backends 文件 |
| 各处 | `[umd]` / `[umdio]` / `[hletrace]` / `[mem]` / `[exit]` / `[cache]` 诊断埋点 |

### 5. 外部依赖补丁（不在本仓库）

- **SDL3**（`F:/SWITCH-psp/sdl-src`）：`scripts/switch/deps/sdl3-switch-vulkan-and-touch.patch`
  - Switch 驱动支持 Vulkan 外部上下文（不调 EGL）
  - **触摸修复**：`SDL_SendTouch` 第 5 个参数是**事件类型**不是 bool；
    原代码传 `true/false` 等于 `type=0/1`，内部 `down = (type == SDL_EVENT_FINGER_DOWN)` 恒为 false
    → UI 只有高亮、永远不触发点击

---

## 二、合并上游时的操作要点

1. **先确认上游 tip**：`git fetch` 后用 `origin/master` 引用，不要读工作树（本地 master 可能是旧快照）
2. **整仓 merge 优于逐个 cherry-pick**：本仓库保留完整上游历史，直接 `git merge` 即可
   （2026-10-30 一次合并 2361 个提交、39 个冲突文件，可行且比手工搬补丁可靠）
3. **冲突解决后验证**：`sceVideocodec.cpp` / `sceMpegbase.cpp` / `sceMp4.cpp` 这类
   **我们没有改过的文件，应与上游逐字节一致**（用 `git diff <upstream> -- <file>` 确认）
4. **合并会悄悄丢 include**（例：`sceKernelModule.cpp` 丢 `sceKernelMemory.h`）→ 比对上游 include 列表补回
5. **合并会产生重复定义**（例：`scePower.cpp` 的 `PowerScaleFromDefaultClock` 被复制两遍）→ 删掉一份
6. **上游改函数签名会让我们的诊断代码失效**（例：`sceUmdDeactivate(u32, u32 namePtr)` 不再有 `name`）→ 就地补声明

详细操作手册：[`merge-upstream-manual.md`](merge-upstream-manual.md)

---

## 三、验证改动是否还在（编译后）

```bash
# ELF 符号
nm $BUILD_DIR/PPSSPPSDL.elf | grep -E 'AdoptDiscTitleForHost|__UmdInsert|UmdReplace|DetectUmdDiscType|ReloadShaderCache'

# NRO 字符串（诊断埋点）
strings $BUILD_DIR/PPSSPP.nro | grep -E '\[umd\]|\[umdio\]|\[hletrace\]|\[exit\]|\[mem\]'
```
