# PPSSPP for Nintendo Switch · 移植说明

把 PPSSPP（PSP 模拟器）编译成 Switch 可运行的 NRO，并在官方主线之上增加了
**XMB 内 UMD 热插拔**等能力，同时持续同步上游主线。

> 🤖 想让 AI 工具自动复刻？看 [`AI-REPLICATE.md`](AI-REPLICATE.md)。

---

## 当前状态（2026-11-01 真机验证）

| 功能 | 状态 |
|---|---|
| Vulkan（Mesa NVK）后端 | ✅（默认后端，已做一次性迁移） |
| XMB 界面 | ✅ |
| **XMB 内插入 / 弹出 UMD** | ✅（本移植独有） |
| **UMD Video 视频盘播放** | ✅ 真机验证通过 |
| PSP 游戏 ISO 运行 | ✅ |
| HOME 键返回 XMB | ✅ |
| 游戏帧率 | ✅ 正常（XMB 启动卡顿已随大合并消失） |
| PMF 过场动画 | ✅（依赖 FFmpeg 57） |
| 触屏点击 | ✅（依赖 SDL3 补丁） |
| **界面中文（TTF 回退字体）** | ✅ 缺字不再显示问号 |
| **金手指数据库全量汉化** | ✅ 83,467 条（含 329 条坏行修复） |
| **NSP 桌面快捷方式 ×2** | ✅ XMB 直进版（带 `--vsh`）+ 主界面版 |
| **--vsh 自动装固件** | ✅ 固件缺失时自动装 660.PBP/EBOOT.PBP，装完自动删包 |
| **数据目录跟随 NRO** | ✅ 不再创建 `/switch/ppsspp-nx`（含旧目录自动迁移） |
| **日志开关 + 时间戳** | ✅ 设置→工具，默认关；逃生口 `ppsspp_debug.txt` |
| **APM 满频性能档** | ✅ 启动时请求 1785MHz |
| XMB 进游戏偶发 init memory fail | ✅ 已修（内存映射自动重试） |
| 退出时 Switch 弹「由于发生错误，软件已关闭」 | ⚠️ 已知问题，次要，重进即可（见下） |

**版本**：1.20.4（与主线开发版一致，跟随主线手动更新）；署名：龙魂猪猪（基于 NaGaa95 版本修改）。
**当前产物**：`output/PPSSPP.nro`，73,805,206 字节，md5 `e76d1bc63585e0152e0473f11d378c4d`（更新检查器指向本仓库版）。
已合并上游主线 tip `61865a5c`（2026-10-30），即一次性吃进 2361 个上游提交。
**发布**：https://github.com/jackzhang153759/ns-psp/releases（tag `v1.20.4`，含 NRO + XMB/MainMenu 两个 NSP 快捷方式）。

---

## 血统

```
hrydgard/ppsspp  (官方主线)
        │
        ├── NaGaa95/ppsspp-nx  (Switch / libnx + Vulkan 移植)
                    │
                    └── 本仓库  (+ XMB UMD 热插拔 / 宿主身份同步 / Switch 适配)
```

本仓库**保留完整上游历史**，因此可以直接 `git merge` 上游 tip，
不需要手工 cherry-pick 补丁——这是 2026-10-30 验证过的最优路线。

---

## 快速开始

### 已有环境（我们自己的机器）

```bash
# 编译
/c/devkitPro/msys2/usr/bin/bash.exe -lc "bash /f/SWITCH-psp/ppsspp-nx/scripts/switch/build.sh"

# 检查上游有无更新
bash scripts/switch/sync-upstream.sh --check

# 拉取并合并上游，然后自动编译验证
bash scripts/switch/sync-upstream.sh --merge --build
```

### 从零开始

见 [`AI-REPLICATE.md`](AI-REPLICATE.md) 第 3 节（Mesa SDK / SDL3 / FFmpeg 57 的搭建）。

---

## 目录地图

| 路径 | 作用 |
|---|---|
| `AI-REPLICATE.md` | **AI 一键复刻入口** |
| `scripts/switch/build.sh` | 编译 NRO |
| `scripts/switch/sync-upstream.sh` | 同步主线 + 合并 + 编译验证 |
| `scripts/switch/deps/` | 依赖构建脚本（Mesa / FFmpeg 57 / SDL3 patch / 子模块） |
| `docs/switch/pitfalls.md` | ⚠️ **踩坑清单**，编译失败先查这里 |
| `docs/switch/our-changes.md` | 我们相对上游的改动（合并冲突时必读） |
| `docs/switch/vsh-umd-insert.md` | VSH UMD 插入功能的移植说明 |
| `docs/switch/merge-upstream-manual.md` | 合并主线的操作手册 |
| `docs/switch/UPSTREAM-BASE` | 已合并的上游 commit（sync 脚本使用） |
| `../HANDOFF.md` | 更早的技术交接文档（历史背景，部分遗留问题已作废） |

---

## 部署

1. 把 `PPSSPP.nro` 拷到 SD 卡 `SD:/switch/ppsspp/PPSSPP.nro`
2. **XMB 需要固件**：已有固件的机器直接用；没有的话把官方 6.60 升级包
   （改名为 `660.PBP` 或 `EBOOT.PBP`）放到 PPSSPP 目录，用 XMB 快捷方式启动一次，
   会自动安装并删包。也可以在 XMB 里走固件安装界面手动装。
3. 日志会写到 SD 卡根目录 `sdmc:/ppsspp_log.txt`（设置→工具里开启后生效，默认关）
4. NSP 快捷方式生成器：`../tools/make_forwarder_nsp.py`（需要 prod.keys，**keys 绝不能外传**）

---

## 三条铁律

1. **FFmpeg 必须是 57**（libavcodec 57.24.102），不能用 devkitPro 的 7.x —— 否则 PMF 花屏且播完必卡死。
2. **构建目录必须在 C 盘 NTFS**，放 F 盘（exFAT）会让 cmake `try_compile` 失败。
3. **合并上游时保住我们的改动**：`Core/HLE/sceUmd.cpp`、`Core/Loaders.cpp`、
   `Core/HLE/sceKernelModule.cpp` 等的 Switch 适配逻辑优先于上游版本，
   编译报错只做最小修补，不删逻辑。详见 `docs/switch/our-changes.md`。

---

## 已知问题 / 未修复 BUG（2026-11-01 v1.20.4.1 更新）

按影响排序。

1. **退出时 Switch 弹「由于发生错误，软件已关闭」**（次要，用户接受）
   退出 PPSSPP（含 HOME 回桌面）时偶发系统报错弹窗，软件本体功能不受影响，重进即可。
   相关埋点已在 `SDL/SDLMain.cpp`（`[exit] __nx_win_exit` 防重入 + `__libnx_exception_handler`），
   排查看日志的 `[mem] svcMapProcessCodeMemory rc=` 与 `[exit]` 组。疑似 libnx/SDL 退出时序问题，未根治。
2. **从 XMB 启动的游戏不会切换成就集**
   `Achievements::SetGame()` 仍走 XMB 路径调用，RetroAchievements 用户从 XMB 进游戏时成就不生效。
3. **游戏内 Reset 行为变化**：现在重启当前游戏（旧行为是回 XMB）；
   `__DisplayResetFrameTiming()` 对 XMB 路径是无效代码。
4. **合金弹头合集（MS1~MS6）金手指会触发游戏自崩**
   该合集内嵌 Neo-Geo 模拟器，启动时有 work RAM 自检；旧版金手指写自检区导致
   `work ram error`（实测 read 03e7 ≠ write 5555）。**不是 PPSSPP 的 BUG**，
   需要针对该合集重新抓地址，搁置中（其他游戏金手指正常）。
5. **Switch 版内置 AdHoc 服务器不含 aemu 数据中继**
   内置服务器提供大厅撮合，但 aemu_postoffice 数据中继因 enet 符号冲突未链入 Switch 构建
   （`sceNetAdhoc.cpp` `#if !PPSSPP_PLATFORM(SWITCH)`）。**局域网联机走 P2P 直连模式即可**
   （两台设备的包中继都设为禁用），中继模式仅供连接外部中继服务器时使用。
6. **FastLoad boost 的 GPU 取舍**（仅未装 sys-clk 的机器）
   启动时请求的 CPU 满频档在部分机型会压低 GPU 频率。装了 sys-clk 的用户可自行覆盖；
   如需彻底规避可加"性能档开关"，未做。
7. **Mesa 驱动落后于上游 Mesa**（可选优化项，非 BUG）
   升级可行性报告见 `docs/switch/mesa-upgrade-feasibility.md`，待用户决定是否跟进。

---

## 局域网联机快速指南（Switch ↔ 安卓/PSP 等）

1. **端口偏移**：所有联机设备的 Port Offset 都设为 **10000** 且保持一致
   （小于 1024 的端口会被系统拒绝，游戏内部端口经常是 1，见 `docs/switch/pitfalls.md` F11）
2. **服务器**：选一台设备当服务器——它填 `localhost` + 勾选「启用内置的 Ad Hoc 服务器」+
   启用联网；其他设备填这台机器的**局域网真实 IP**（联机服务器列表里能看到本机 IP）
3. **中继**：局域网联机两边都把「使用服务器提供的包中继」设为**禁用**（P2P 直连，延迟最低）；
   只有连外部中继服务器（如 socom.cc）时才开中继
4. **时序**：服务器那台先启动游戏（内置服务器随游戏启动/销毁），客户端再连
