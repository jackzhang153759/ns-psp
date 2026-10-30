# PPSSPP Switch · VSH 下插入/弹出 UMD 功能移植说明

## 目标

在 NaGaa95 的 **Vulkan 版**（`ppsspp-nx`，游戏能跑、触屏正常）基础上，补上「XMB（VSH）下插入/弹出 UMD 光盘」功能。

这个功能在我之前编译的 OpenGL ES 版里已经能用，但 OpenGL ES 版游戏跑不了（黑屏），所以要把它移植到能跑游戏的 Vulkan 版上。

---

## 改动内容（7 个文件，50 行新增）

已导出为 `patches/ppsspp-nx-vsh-umd-insert.patch`。

| 文件 | 改动 |
|---|---|
| `Core/HLE/sceKernelModule.h/.cpp` | 新增 `__KernelIsRunningVSH()`（返回 `g_runningVSH`，检测是否运行 XMB/VSH） |
| `Core/HLE/sceUmd.h/.cpp` | 新增 `__UmdIsInserted()`（UMD 是否已插入）和 `__UmdEject()`（弹出 UMD，含 unmount umd0:/umd1:/umd:/disc0:） |
| `UI/PauseScreen.cpp` | 加 `__KernelIsRunningVSH()` 分支：XMB 下根据 UMD 状态显示「插入 UMD」/「弹出 UMD」菜单 |
| `assets/lang/en_US.ini` | 补「Eject UMD」「Insert UMD」翻译 key |
| `assets/lang/zh_CN.ini` | 补「弹出UMD光盘」「插入UMD光盘」中文翻译 |

---

## 核心逻辑

原版 NaGaa95 的 PauseScreen 只有：
```cpp
if (getUMDReplacePermit()) {   // 依赖固件调用 sceUmdReplacePermit()，VSH 下不触发
    "Switch UMD" 菜单
}
```

改为（对照更新主线）：
```cpp
if (__KernelIsRunningVSH()) {          // 检测到正在运行 XMB
    if (__UmdIsInserted()) {
        "Eject UMD" → __UmdEject()     // 弹出
    } else {
        "Insert UMD" → UmdReplaceScreen // 插入（选 ISO 挂载）
    }
} else if (getUMDReplacePermit()) {
    "Switch UMD"                        // 游戏内切换（原功能保留）
}
```

---

## 为什么这样改

- `__KernelIsRunningVSH()` / `__UmdIsInserted()` / `__UmdEject()` 这三个函数是 **PPSSPP 主线后来新加的**，NaGaa95 版基于更老的主线，所以没有。
- 我是从「更新主线」（`ppsspp-upstream`）里精确对照移植的，逻辑与主线一致，是经过验证的。

---

## 编译（尚未完成）

**代码已改完，但编译 Vulkan 版需要 Mesa 26.2.2 SDK，尚未构建。**

NaGaa95 的 `scripts/build-switch.sh` 要求：
- `MESA_VERSION="26.2.2"`，`MESA_REVISION="67e632f90cb18a5af10f020cdf671ea262ad9a0b"`
- SDK 来源：`danfromtico/mesa-switch`（公开仓库，但无官方预编译 Release，需自己构建）

构建 Mesa SDK 的两条路：
1. **Docker**：`mesa-switch` 的 `build-switch.sh`（仅 Vulkan，只需 Docker）
2. **MSYS2 + Rust**：`build-unified.sh`（Vulkan+GL，需 MSYS2 mingw64 + Rust stable-x86_64-pc-windows-gnu + aarch64-unknown-linux-gnu target）

**待办**：搭 Mesa SDK 环境 → 编译 Vulkan 版 → 出 nro → 测试。

---

## 测试验证点（编译出来后）

1. 装 6.60 固件 → 进 XMB
2. 暂停模拟 → 菜单里出现「插入 UMD」
3. 选「插入 UMD」→ 选一个 ISO → 能挂载
4. 再次暂停 → 变成「弹出 UMD」→ 能弹出
5. 游戏能正常运行（Vulkan 后端）
6. 触屏正常
