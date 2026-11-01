# PPSSPP for Nintendo Switch（ns-psp）

**PSP 模拟器 PPSSPP 的 Switch 移植版** —— 带 XMB 原生界面、UMD 热插拔、UMD Video 播放与全中文界面。

> 🎮 **下载**：见 [Releases](https://github.com/jackzhang153759/ns-psp/releases)（NRO 本体 + 两个桌面快捷方式 NSP）
> 🤖 **AI 复刻**：想让 AI 工具自动复刻编译？看 [AI-REPLICATE.md](AI-REPLICATE.md)
> ⚠️ **编译踩坑**：编译失败先查 [docs/switch/pitfalls.md](docs/switch/pitfalls.md)

---

## 这个项目基于什么

```
hrydgard/ppsspp          PPSSPP 官方主线（GPL 2.0+，作者 Henrik Rydgård 及社区贡献者）
        │                  已合并 2026-10-30 的主线 tip 61865a5c（一次性吃进 2361 个上游提交）
        ▼
NaGaa95/ppsspp-nx        Switch / libnx 移植（Vulkan / Mesa NVK 图形后端）
        │
        ▼
本仓库 ns-psp             ✏️ 修改者：龙魂猪猪（基于 NaGaa95 版本修改）
                           在 Switch 移植之上叠加了大量功能修复与体验优化（见下文）
```

- **版本号**：1.20.4（与 PPSSPP 主线开发版一致，随主线手动跟进）
- **模拟器核心**：完全继承上游 PPSSPP，无需 BIOS 文件（HLE 模拟）
- **Switch 图形后端**：Vulkan（Mesa NVK 驱动，自建 Mesa SDK 交叉编译）
- **上游许可证**：GPL 2.0 及更高版本，本仓库同样遵循

---

## ✅ 已完成功能（均经真机验证）

### 核心模拟
| 功能 | 说明 |
|---|---|
| PSP 游戏 ISO 运行 | ✅ 帧率正常 |
| **UMD Video 视频盘播放** | ✅ 本移植亮点：XMB 里直接播 UMD 电影盘 |
| **XMB 内插入 / 弹出 UMD** | ✅ 本移植独有：不退出 XMB 换盘 |
| PMF 过场动画 | ✅（依赖自编 FFmpeg 57，主线 7.x 会导致花屏+卡死） |
| HOME 键返回 XMB | ✅ |
| 触屏点击 | ✅（SDL3 Switch 驱动补丁） |
| Vulkan 图形后端 | ✅ 默认后端（旧配置自动一次性迁移） |

### 本仓库新增 / 定制
| 功能 | 说明 |
|---|---|
| **界面全中文** | 启用 SDL3_ttf + 思源黑体回退，缺字不再显示问号 |
| **金手指数据库全量汉化** | 83,467 条金手指名翻译成中文（并修复原库 329 条坏行） |
| **NSP 桌面快捷方式 ×2** | XMB 直进版（带 `--vsh` 参数）+ 主界面版，图标独立 |
| **XMB 自动装固件** | 固件缺失时自动安装目录里的 660.PBP / EBOOT.PBP，**装完自动删包** |
| **数据目录跟随 NRO** | 不再固定创建 `/switch/ppsspp-nx`，旧目录自动迁移（含 games/Library） |
| **日志系统** | 设置→工具 开关（默认关）+ 每行时间戳 + 日志文件 `sdmc:/ppsspp_log.txt` |
| **APM 满频性能档** | 启动自动申请 CPU 1785MHz |
| **init memory fail 修复** | XMB 进游戏偶发内存初始化失败 → 自动重试 |
| **应用内检查更新** | 自动拉取本仓库最新 Release 的 NRO |
| **品牌** | 启动页署名"龙魂猪猪基于NaGaa95版本修改"，版本号与主线一致 |
| **XMB 提示修正** | 暂停菜单提示改为"按下ZL打开暂停菜单" |

---

## ❌ 未完成 / 已知问题（截至 v1.20.4）

按影响排序，详见 [`SWITCH-PORT.md`](SWITCH-PORT.md)：

1. **退出时 Switch 弹「由于发生错误，软件已关闭」**（次要）——重进即可，疑似 libnx/SDL 退出时序，未根治
2. **从 XMB 启动的游戏不切换成就集**——RetroAchievements 成就不随游戏切换
3. **游戏内 Reset 行为**——现在重启当前游戏（旧版是回 XMB）
4. **合金弹头合集金手指会触发游戏自崩**——游戏内嵌 Neo-Geo 模拟器的内存自检与旧地址冲突，非模拟器 BUG，需重抓地址（已搁置）
5. ** Mesa 驱动落后于上游**——升级可行性见 `docs/switch/`，待跟进
6. **FastLoad 满频档在无 sys-clk 的机器可能压 GPU 频率**——可选加开关，未做

---

## 📦 安装使用

1. **模拟器本体**：`PPSSPP.nro` 放到 SD 卡 `SD:/switch/ppsspp/PPSSPP.nro`
2. **固件**：已有固件的机器直接用；没有的话把官方 6.60 升级包（改名 `660.PBP` 或 `EBOOT.PBP`）放进 PPSSPP 目录，用 **XMB 快捷方式**启动一次即自动安装并删包
3. **桌面快捷方式**（可选）：用 DBI / Awoo 等工具安装 Release 里的两个 NSP
4. **游戏 ISO**：放 `SD:/switch/ppsspp/games/`（XMB 和主界面都能读）
5. **日志排查**：设置→工具→日志 打开后重启，日志在 SD 卡根目录 `ppsspp_log.txt`

---

## 🔨 编译（进阶）

```bash
# 日常编译（需要 devkitPro + 自建 Mesa SDK / FFmpeg 57 / SDL3，详见 AI-REPLICATE.md）
/c/devkitPro/msys2/usr/bin/bash.exe -lc "bash scripts/switch/build.sh"

# 检查 / 合并上游主线
bash scripts/switch/sync-upstream.sh --check
bash scripts/switch/sync-upstream.sh --merge --build
```

环境搭建从零开始：[AI-REPLICATE.md](AI-REPLICATE.md)；所有踩坑记录：[docs/switch/pitfalls.md](docs/switch/pitfalls.md)。

---

## 📚 文档索引

| 文档 | 内容 |
|---|---|
| [SWITCH-PORT.md](SWITCH-PORT.md) | 项目状态 / 部署 / 已知问题全量盘点 |
| [AI-REPLICATE.md](AI-REPLICATE.md) | AI 一键复刻编译步骤 |
| [docs/switch/pitfalls.md](docs/switch/pitfalls.md) | 踩坑清单（编译失败先查） |
| [docs/switch/our-changes.md](docs/switch/our-changes.md) | 相对上游的全部改动 |
| [docs/switch/merge-upstream-manual.md](docs/switch/merge-upstream-manual.md) | 合并主线操作手册 |
| [README-upstream.md](README-upstream.md) | 上游 PPSSPP 原版说明 |

---

## 🤖 关于本项目 · AI 编写声明

本项目**绝大部分程序代码由 AI（大语言模型）编写**；项目的**创意、需求规划、功能决策与全部真机测试由 [龙魂猪猪] 主导完成**。在此明确公示：

- 本仓库**就是完整源代码**，全部以 GPL 2.0 开放，任何人可自由查阅、使用、修改，无需向任何人索取；
- 我们没有也无法保留"私有改动"——所有修改都已提交在 Git 历史中；
- 欢迎提交 Issue 与 Pull Request。

---

## 📄 许可证

GPL 2.0 及更高版本（跟随上游 PPSSPP）。
- PPSSPP：© Henrik Rydgård 及社区贡献者
- Switch 移植：基于 NaGaa95/ppsspp-nx
- 本仓库的修改与定制：**龙魂猪猪**
- 上游原版说明见 [README-upstream.md](README-upstream.md)
