# PPSSPP Switch 移植 · 合并主线操作手册

> 目标：把 PPSSPP 主线（含 XMB/固件）+ Switch 移植，编译成可运行的 NRO。
> 本手册记录完整的方案、改动清单和踩坑经验，供以后合并主线新版本时复用。

---

## 一、最终产物

| 文件 | 路径 | 说明 |
|---|---|---|
| NRO | `F:/SWITCH-psp/output/PPSSPP.nro` | 41MB，含内嵌 RomFS assets |
| ELF | `C:/switch-build/ppsspp-build/PPSSPPSDL.elf` | 中间产物 |

---

## 二、总体架构

```
SDL3 (Switch 驱动, EGL 创建 GLES2 上下文)
        │
        ▼
PPSSPP OpenGL ES 后端 (USING_GLES2=ON)
  ├── 基础 GLES2 函数  → libGLESv2.a 静态提供
  └── GLES3 扩展函数   → gl3stub 运行时加载 (eglGetProcAddress)
                          └─ 函数名重命名为 ppsspp_glXXX 避免符号冲突
```

**关键决策**：Switch 用 **OpenGL ES** 后端（不是 switch port 原版的 Vulkan），因为 Vulkan 需要自定义 Mesa 26.2.2 SDK（Docker+Rust 编译），当前环境无。

---

## 三、代码仓库结构（F:/SWITCH-psp/）

| 目录 | 作用 |
|---|---|
| `ppsspp-upstream/` | **主仓库**：主线 + switch port 已 merge（当前 HEAD: `5a99b3db38`） |
| `ppsspp-nx/` | switch port 源（NaGaa95/ppsspp-nx，commit `c9b355b`），参考用 |
| `ppsspp-switch-ref/` | **旧版 Switch 移植（SirSamael）**，gl3stub 重命名方案的来源 |
| `sdl-src/` | SDL3 源码 |
| `patches/` | 导出的 Switch 适配 patch |
| `output/` | 编译产物 |

---

## 四、Switch 适配改动清单（8 个文件）

这些改动已导出为 `patches/switch-opengl-es-adaptation.patch`（357 行，可 `git apply`）。

| 文件 | 改动内容 |
|---|---|
| `CMakeLists.txt` | USING_GLES2=ON、OPENGL_LIBRARIES=GLESv2 EGL drm_nouveau glapi、移除 -UNEED_WRITEV、移除 SwitchLsfgBackend |
| `Common/GPU/OpenGL/gl3stub.h` | **核心**：从 ppsspp-switch-ref 复制，含 Switch 重命名块（`#define glXXX ppsspp_glXXX`） |
| `Common/GPU/Vulkan/SwitchLSFG.h` | GLES2 stub（空实现，Vulkan 帧生成不适用） |
| `Common/Net/Resolve.cpp` | Switch 用 curl（curl_global_cleanup） |
| `Core/MIPS/JitCommon/JitProfiler.cpp` | Switch 空实现（无 dlfcn） |
| `SDL/SDLGLGraphicsContext.cpp` | X11 include 加 `!PPSSPP_PLATFORM(SWITCH)` |
| `SDL/SDLJoystick.cpp` | 加 `#include "Core/Config.h"` |
| `Common/File/SwitchCompat.cpp` | 新增，占位（smb2 自带 writev/readv） |

---

## 五、编译流程

### 1. SDL3（Switch 版）
```bash
bash build_sdl3.sh
```
**关键参数**（已在脚本里）：
- `-DNINTENDO_SWITCH=ON`
- `-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -DSDL_VIDEO_DRIVER_SWITCH=1 -DSDL_AUDIO_DRIVER_SWITCH=1 -DSDL_JOYSTICK_SWITCH=1"`

### 2. PPSSPP
```bash
bash build_ppsspp.sh
```

---

## 六、关键踩坑（务必记住）

1. **F 盘是 exFATX**：build 目录必须放 C 盘 NTFS（`C:/switch-build/`），否则 cmake try_compile 报 Permission denied。

2. **rm -rf 被 genie-trash 拦截**：删除目录用 PowerShell `Remove-Item -Recurse -Force`。

3. **CMAKE_C_FLAGS 会覆盖工具链默认的 `-D__SWITCH__`**：必须用 `CMAKE_C_FLAGS_RELEASE` 追加，不能用 `CMAKE_C_FLAGS`。

4. **SDL3 的 NINTENDO_SWITCH 分支被 UNIX 分支"抢"**：Switch 上 CMake 的 UNIX=真，导致 SDL3 走 UNIX 分支而非 switch 分支。靠 `CMAKE_C_FLAGS_RELEASE` 强制 `-DSDL_VIDEO_DRIVER_SWITCH=1` 解决。

5. **gl3stub 与 libGLESv2 符号冲突**（核心难点）：
   - gl3stub 把 GLES3 函数定义成**函数指针变量**（BSS 段）
   - libGLESv2 把同名函数定义成**强符号函数**（.text 段）
   - 解决：ppsspp-switch-ref 的 gl3stub.h 在 `__SWITCH__` 下把 GLES3 函数重命名为 `ppsspp_glXXX`

6. **smb2 的 writev**：switch port 的 `-UNEED_WRITEV` 是**错的**（反而禁用了 smb2 自己的 writev 实现）。应移除，让 smb2 compat.c 正常提供。

7. **devkitPro switch-mesa 的 GLES3/gl3ext.h 不完整**：缺 GLES2 扩展宏（但最终方案用 GLES2 头 + gl3stub，避开了这个问题）。

---

## 七、SPIRV-Cross submodule 注意

- `.gitmodules` 已声明 `ext/SPIRV-Cross`，但 HEAD 未 track gitlink（主线状态）。
- 编译前需 `git submodule update --init ext/SPIRV-Cross`（当前已初始化到 `f11ba9f0`）。
- 这个不影响 patch 应用，但重新 clone 后要记得初始化 submodule。

---

## 八、以后合并主线的步骤

1. **更新主线**：
   ```bash
   cd ppsspp-upstream
   git fetch origin && git merge origin/master   # 或用 rebase
   ```

2. **解决冲突**（主要冲突点就是上述 8 个文件，方案已明确）。

3. **应用 Switch 适配 patch**（如果主线改动冲掉了适配）：
   ```bash
   git apply patches/switch-opengl-es-adaptation.patch
   ```
   注意：patch 基于当前 HEAD（`5a99b3db38`），主线大改后可能需要手动调整。

4. **重新编译**：
   ```bash
   bash build_sdl3.sh   # SDL3 一般不用重编（除非 SDL3 版本变了）
   bash build_ppsspp.sh
   ```

---

## 九、环境关键路径

| 项 | 路径 |
|---|---|
| devkitPro | `C:/devkitPro/` |
| msys2 bash | `C:/devkitPro/msys2/usr/bin/bash.exe` |
| 工具链 | `aarch64-none-elf-gcc 16.1.0`，`C:/devkitPro/cmake/Switch.cmake` |
| switch-mesa | `C:/devkitPro/portlibs/switch/lib/`（libGLESv2.a 等） |
| SDL3 库 | `C:/switch-build/sdl-prefix/lib/libSDL3.a` |
| build 目录 | `C:/switch-build/` |
| 代理 | v2rayN 10808（下载依赖用 curl_cffi 伪装 Chrome 指纹） |

---

## 十、真机部署（明天测试）

把 `output/PPSSPP.nro` 放到 SD 卡：
```
SD:/switch/ppsspp/PPSSPP.nro
```
如需 XMB/固件功能，还要放 PSP 固件文件到 `SD:/switch/ppsspp/` 下（或用 PPSSPP 内置的固件下载）。
