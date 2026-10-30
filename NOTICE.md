# 署名与第三方协议声明

## 一、本项目署名

**NSpsp（PPSSPP for Nintendo Switch）移植与修改：龙魂猪猪**

本仓库是在下列开源项目基础上的衍生作品。所有上游作者的版权声明继续有效，
本项目新增与修改的部分同样以 **GPL-2.0-or-later** 授权。

---

## 二、上游与第三方项目

| 项目 | 用途 | 协议 | 链接 |
|---|---|---|---|
| **PPSSPP** | 代码主体（PSP 模拟器） | GPL-2.0-or-later | https://github.com/hrydgard/ppsspp |
| **ppsspp-nx**（NaGaa95） | Switch / libnx + Vulkan 移植基础 | 跟随上游 | https://github.com/NaGaa95/ppsspp-nx |
| **mesa-switch**（danfromtico / NaGaa95） | Mesa 的 Switch（Horizon）GPU 后端，提供 Vulkan(NVK) | Mesa: MIT | https://github.com/danfromtico/mesa-switch |
| **SDL3** | 输入 / 窗口 / 触屏（本项目打了 Switch 补丁） | zlib | https://github.com/libsdl-org/SDL |
| **FFmpeg**（ppsspp-ffmpeg 分支） | 视频解码（本项目要求自编 57.x） | LGPL-2.1 | https://github.com/hrydgard/ppsspp-ffmpeg |
| **devkitPro**（devkitA64 / libnx / portlibs） | 交叉编译工具链 | 各自协议 | https://devkitpro.org |

### 仓库内第三方依赖（`ext/`）部分协议摘录

| 组件 | 协议 | 位置 |
|---|---|---|
| SPIRV-Cross | Apache-2.0 | `ext/SPIRV-Cross/LICENSE` |
| glslang | 多协议（BSD / Apache / MIT 混合） | `ext/glslang/LICENSE.txt` |
| zstd | GPL-2.0（亦提供 BSD 变体） | `ext/zstd/COPYING` |
| FreeType | FTL / GPL-2.0 | `ext/freetype/LICENSE.txt` |
| miniupnpc | BSD-3-Clause | `ext/miniupnp/LICENSE` |
| FFmpeg | LGPL-2.1（多数文件） | `ext/ffmpeg/LICENSE.md` |
| RapidJSON | MIT（Tencent / Milo Yip） | `ext/rapidjson/license.txt` |

完整条款请以各组件目录下的原始许可证文件为准。PPSSPP 自身的许可证见根目录
[`LICENSE`](LICENSE)（= `LICENSE.TXT`）。

---

## 三、本项目相对上游的改动范围

修改者：**龙魂猪猪**

1. XMB 内 UMD 热插拔（`Core/HLE/sceUmd.*`、`Core/Loaders.cpp`、`Core/PSPLoaders.cpp`、
   `Core/HLE/sceIo.cpp`、`UI/PauseScreen.cpp`、`assets/lang/*.ini`）
2. XMB 启动游戏的宿主侧身份同步（`Core/HLE/sceKernelModule.cpp`）
3. UMD Video 播放支持（合并上游主线 + Switch 侧盘类型识别）
4. Switch 平台适配与诊断埋点（`Core/HW/MediaEngine.cpp`、`Common/MemArenaHorizon.cpp`、
   `SDL/SDLMain.cpp`、`UI/NativeApp.cpp` 等）
5. 外部依赖补丁（SDL3 Switch 驱动补丁）
6. 构建脚本与复刻文档（`scripts/switch/`、`docs/switch/`、`AI-REPLICATE.md` 等）

历史差异快照：`scripts/switch/deps/ppsspp-nx-vsh-umd-insert.patch`
逐项说明：`docs/switch/our-changes.md`

---

## 四、免责声明

1. **本仓库不包含任何受版权保护的内容**：
   不含 PSP 固件（使用 XMB 需自备 6.60 固件）、不含游戏 ISO、不含 UMD Video 光盘镜像。
   请自行合法获取，勿用于盗版。
2. **仅在自制固件（Custom Firmware）环境下运行**。使用前请确保你有权修改自己的设备。
3. **按"原样"提供，不附带任何担保**。因使用本软件造成的任何损失，作者不承担责任。
4. 本项目与任天堂、索尼无任何关联。"Nintendo Switch""PSP""UMD" 等为各自权利人的商标。
