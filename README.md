# NSpsp — PPSSPP for Nintendo Switch

把 PSP 模拟器 **PPSSPP** 编译成 Nintendo Switch 自制固件可运行的 **NRO**，并在官方主线之上增加了
**XMB 内 UMD 热插拔**、**UMD Video 视频盘播放** 等能力，同时持续同步上游主线。

> 📦 **当前版本真机验证通过（2026-10-30）**：UMD Video 能播、PSP 游戏 ISO 能跑、
> HOME 键返回 XMB 正常、帧率正常。

---

## 一、本项目基于哪些项目

| 项目 | 作用 | 协议 |
|---|---|---|
| [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp) | 官方主线（本仓库的代码主体） | GPL-2.0-or-later |
| [NaGaa95/ppsspp-nx](https://github.com/NaGaa95/ppsspp-nx) | Switch / libnx + Vulkan(NVK) 移植基础 | 跟随上游 |
| [NaGaa95/mesa-switch](https://github.com/NaGaa95/mesa-switch)（上游 [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)） | Mesa 的 Switch（Horizon）GPU 后端，提供 `libvulkan.a` | Mesa: MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | 输入/窗口/触屏（**需打本项目的 Switch 补丁**） | zlib |
| [FFmpeg](https://github.com/hrydgard/ppsspp-ffmpeg) | 视频解码（**必须自行编译 57.x**） | LGPL-2.1 |
| devkitPro（devkitA64 / libnx / portlibs） | 交叉编译工具链 | 各自协议 |

**本仓库 = 上游 PPSSPP 主线 + Switch 移植基础 + 我们的改动**。
本仓库保留完整上游历史，因此后续同步主线采用 **`git merge` 整仓合并**，而非手工搬补丁
（2026-10-30 一次性合并 2361 个上游提交验证可行）。

---

## 二、我们做了哪些改动

修改者：**龙魂猪猪**

### 核心功能（相对上游新增）

1. **XMB 内插入 / 弹出 UMD 光盘镜像**
   原版只能从 PPSSPP 自己的菜单启动游戏，不能在 PSP 的 XMB 界面里换盘。
   涉及 `Core/HLE/sceUmd.cpp/.h`、`Core/Loaders.cpp`、`Core/PSPLoaders.cpp`、
   `Core/HLE/sceIo.cpp`、`UI/PauseScreen.cpp`、`assets/lang/*.ini`。
2. **XMB 启动游戏时的宿主侧身份同步**
   从 XMB 的 UMD 槽启动游戏走的是被模拟 PSP 内部的 `sceKernelLoadExec`，**不经过 `CPU_Init`**，
   因此宿主侧的 disc ID、兼容配置、内存布局、着色器缓存路径都必须手动补齐
   （`Core/HLE/sceKernelModule.cpp` 的 `AdoptDiscTitleForHost()` / `RestoreShellTitleForHost()`）。
3. **UMD Video 视频盘播放**
   通过合并上游主线实现，并配套 Switch 侧的盘类型识别（`DetectUmdDiscType()` 解析 `UMD_DATA.BIN`）。
4. **Switch 平台适配与诊断埋点**
   解码线程数、`MemArena` 诊断、日志落 SD 卡、退出防重入，以及
   `[umd]` / `[umdio]` / `[hletrace]` / `[mem]` / `[exit]` / `[cache]` 六组诊断标记。

### 外部依赖补丁

- **SDL3 Switch 驱动**（`scripts/switch/deps/sdl3-switch-vulkan-and-touch.patch`）
  - 支持 Vulkan 外部上下文（不调 EGL）
  - **触屏修复**：`SDL_SendTouch` 第 5 个参数是**事件类型**不是 bool；原代码传 `true/false`
    等于 `type=0/1`，内部 `down` 恒为 false → UI 只有高亮、永远点不动
- **FFmpeg 必须自行编译 57.x**（见下"三条铁律"）

### 工程化（本次新增）

- `AI-REPLICATE.md`：**给 AI 工具的一键复刻手册**
- `docs/编译操作手册.md`：给人类的**详细无坑操作手册**
- `docs/switch/pitfalls.md`：踩坑清单
- `docs/switch/our-changes.md`：改动红线清单（合并上游时必读）
- `scripts/switch/build.sh`、`scripts/switch/sync-upstream.sh`：编译与主线同步脚本
- `scripts/switch/deps/`：依赖构建脚本（Mesa SDK / FFmpeg 57 / SDL3 补丁 / 子模块）

完整差异快照：`scripts/switch/deps/ppsspp-nx-vsh-umd-insert.patch`

---

## 三、功能状态

| 功能 | 状态 | 备注 |
|---|---|---|
| Vulkan（Mesa NVK）后端 | ✅ | |
| XMB 界面 | ✅ | 需自备 6.60 固件 |
| **XMB 内 UMD 热插拔** | ✅ | 本项目独有 |
| **UMD Video 视频盘播放** | ✅ | 2026-10-30 真机验证 |
| PSP 游戏 ISO 运行 | ✅ | |
| HOME 键返回 XMB | ✅ | |
| 游戏帧率 | ✅ | XMB 启动的卡顿问题已消失 |
| PMF 过场动画 | ✅ | 依赖 FFmpeg 57 |
| 触屏点击 | ✅ | 依赖 SDL3 补丁 |
| 退出时弹「由于发生错误，软件已关闭」 | ⚠️ | **已知问题**，重进即可，未修 |

已知问题详见 [`docs/已知问题.md`](docs/已知问题.md)。

---

## 四、怎么复刻出 NRO

### 路线 1：交给 AI（推荐）

把 [`AI-REPLICATE.md`](AI-REPLICATE.md) 整个喂给你的 AI 编码工具（Claude / Codex / Cursor 等），
让它按文档执行。该文档每一步都写了"要执行的命令""预期结果""失败怎么办"。

### 路线 2：自己动手

按 [`docs/编译操作手册.md`](docs/编译操作手册.md) 一步步做，该手册已把 12+ 个坑写成"预防式"步骤，
照做即可一次编过。

**两条路线的共同前提（三条铁律）**：

1. **FFmpeg 必须是 57（libavcodec 57.24.102）**，绝不能用 devkitPro 的 `switch-ffmpeg`（7.x）。
   用错 → PMF 过场动画花屏，且**动画播完必定卡死**。
2. **构建目录必须在 C 盘 NTFS**，放 exFAT 盘会让 cmake 的 `try_compile` 失败。
3. **所有构建命令必须用 devkitPro 的 msys2 bash**（`cmake` 只装在那里）。

---

## 五、目录结构

```
NSpsp/
├── README.md                 本文件
├── AI-REPLICATE.md           ★ 给 AI 的复刻手册
├── NOTICE.md                 署名与第三方协议声明
├── CHANGELOG.md              版本记录
├── LICENSE                   GPL-2.0-or-later（= LICENSE.TXT）
├── docs/
│   ├── 编译操作手册.md        ★ 给人类的一步一步操作手册
│   ├── 已知问题.md
│   ├── upstream-README.md    上游 PPSSPP 的原始 README
│   └── switch/
│       ├── pitfalls.md       ★ 踩坑清单（编译失败先查这里）
│       ├── our-changes.md    ★ 我们的改动红线（合并上游必读）
│       ├── vsh-umd-insert.md VSH UMD 插入功能移植说明
│       ├── merge-upstream-manual.md  合并主线操作手册
│       └── UPSTREAM-BASE     已合并的上游 commit（同步脚本用）
├── scripts/switch/
│   ├── build.sh              编译 NRO
│   ├── sync-upstream.sh      同步主线 + 合并 + 编译验证
│   └── deps/                 Mesa SDK / FFmpeg57 / SDL3 补丁 / 子模块脚本
├── Core/ Common/ GPU/ UI/ SDL/ ...    PPSSPP 源码主体
└── ext/                      第三方依赖
```

---

## 六、同步上游主线

```bash
bash scripts/switch/sync-upstream.sh --check          # 只检查落后多少提交
bash scripts/switch/sync-upstream.sh --merge           # 拉取并合并
bash scripts/switch/sync-upstream.sh --merge --build   # 合并后自动编译验证
```

---

## 七、署名

- **本项目移植与修改：龙魂猪猪**
- 上游项目：`hrydgard/ppsspp`（Henrik Rydgård 等）、`NaGaa95/ppsspp-nx`、
  `danfromtico/mesa-switch` / `NaGaa95/mesa-switch`、SDL3、FFmpeg、devkitPro

完整第三方协议与免责声明见 [`NOTICE.md`](NOTICE.md)。

---

## 八、开源协议

本项目是 GPL-2.0-or-later 项目（PPSSPP）的衍生作品，
**本项目新增与修改的部分同样以 GPL-2.0-or-later 授权**，完整条款见 [`LICENSE`](LICENSE)。

第三方组件各自遵循其原有协议（Mesa MIT、SDL3 zlib、FFmpeg LGPL-2.1、
SPIRV-Cross Apache-2.0、miniupnp BSD-3-Clause 等），详见 [`NOTICE.md`](NOTICE.md)。

> ⚠️ **本仓库不包含任何受版权保护的内容**：不含 PSP 固件（XMB 需自备 6.60 固件）、
> 不含游戏 ISO、不含 UMD Video 光盘镜像。请勿用于盗版。
