# 切片 46 收口报告 — Frameless 主窗口任务栏图标丢失修复

> 版本：v1.0（2026-09-05）｜切片 46（单 bug 修复）｜ctest 346/346 全绿（5m25s）｜构建 0 报错

## 1. 症状（截图实证）

用户截图两联：

| 截图 | 场景 | 任务栏图标 |
|---|---|---|
| @image#1 | 登录窗口显示 | ✅ `:/icons/app_logo.svg` 正常（闪电 logo） |
| @image#2 | 登录成功跳转主窗口 | ❌ Windows 默认空白方块 |

## 2. 根因链

**Qt 已知行为 + 切片 45 `QMainWindow` 接入路径叠加触发**：

1. `WindowChrome` 构造内部执行 `target->setWindowFlags(windowFlags() | Qt::FramelessWindowHint)`（WindowChrome.cpp L14）——**第二次** setWindowFlags（MainWindow/LoginDialog 构造开头各已有一次）。
2. Qt 5.15 中 `setWindowFlags` 对已建 native window 的窗口会**销毁并重建 native window handle（HWND）**；重建只迁移部分 widget 状态，**HICON 不在其列**。
3. 普通路径（`QDialog`）重建后 Qt 在 `show()` 时会自动把 `QWidget::windowIcon()` 重新 apply 到新 HWND → **登录窗正常**。
4. `QMainWindow` 路径特殊：先 `setMenuWidget(tb)`（自绘 TitleBar 挂内部 menubar，已扰动 native window 一次），随后 WindowChrome 的 setWindowFlags **再次重建**。Qt 5.15 在此「menuWidget + 二次重建」组合下**不把 windowIcon 写回新 HWND 的 HICON** → 任务栏显示 Windows 默认空白图标。**主窗丢图标**。

> 一句话：**不是 icon 资源丢了，是 native HWND 的 HICON 没被写回**。Qt 内存态 `QWidget::windowIcon()` 始终存在，任务栏只认 HWND 的 HICON。

## 3. 修复方案（方案 A：WindowChrome 收口强制 reapply）

`WindowChrome` 新增第 4 参 `appIcon`，在 setWindowFlags 重建之后显式 `target->setWindowIcon(appIcon)`——`QWidget::setWindowIcon` 会同步 `windowHandle()->setIcon()`（Windows 触发 `WM_SETICON`），任务栏图标恢复。**所有 Frameless 窗口接入者自动受益，无需各自记住补丁**。

```cpp
// WindowChrome.h
WindowChrome(QWidget* target, TitleBar* titleBar, bool resizable = true,
             const QIcon& appIcon = QIcon());   // 新增（旧签名向后兼容）

// WindowChrome.cpp —— setWindowFlags 重建后强制写回 HICON
if (!appIcon.isNull()) {
    target->setWindowIcon(appIcon);
}
```

> ⚠️ 实现要点：此时**不能**读 `target->windowIcon()` 兜底——重建瞬间 Qt 内存态已被内部清空，必须由调用方显式传入。

## 4. 改动清单

| 文件 | 改动 |
|---|---|
| `apps/ens_app/src/ui/common/WindowChrome.h` | 构造第 4 参 `const QIcon& appIcon = QIcon()`；include `<QIcon>`；注释沉淀切片 46 背景 |
| `apps/ens_app/src/ui/common/WindowChrome.cpp` | setWindowFlags 后 `if (!appIcon.isNull()) target->setWindowIcon(appIcon)` |
| `apps/ens_app/src/ui/main/MainWindow.cpp` | 调用点传 `QIcon(":/icons/app_logo.svg")`（L64 附近） |
| `apps/ens_app/src/ui/auth/LoginDialog.cpp` | 调用点传 `QIcon(":/icons/app_logo.svg")`（L39 附近） |
| `tests/unit/test_chrome_render.cpp` | +3 用例（见 §5），include `<QWindow>`/`<QIcon>` |

## 5. 验证矩阵

### 5.1 新增单测（offscreen 可自动观测 native 层 icon）

native HICON 无法在 offscreen 直接读，但 Qt 写回链路在 native 层有状态可观测：`QWindow::icon()`（`windowHandle()->icon()`）。修复前该值为 null（重建后未写回），修复后非空——作为**自动守卫点**，有人删掉 reapply 即回归红灯。

| 用例 | 断言 | 结果 |
|---|---|---|
| MainWindow restores taskbar icon on WindowChrome HWND rebuild (s46) | `windowHandle()` 非空 && `wh->icon()` 非空 && 中心像素 == app_logo | ✅ |
| LoginDialog restores taskbar icon on WindowChrome HWND rebuild (s46) | 同上（Dialog 路径） | ✅ |
| WindowChrome without appIcon keeps old signature working (s46) | 不传第 4 参仍可构造、`helper()` 非空（向后兼容） | ✅ |

### 5.2 回归

| 层级 | 范围 | 结果 |
|---|---|---|
| 定向 | `ens_tests "[chrome]"` | ✅ 8/8（37 assertions） |
| 全量 | `ctest --test-dir build/vs2022-debug-local --output-on-failure` | ✅ **346/346（0 failed）** |

### 5.3 待用户手动目视（桌面平台）

1. 启动：`bin\Debug\ens_app.exe --point-table data\sim_pointtable_full.json`（sim 未起时主窗断链红标属预期，不影响图标验证）
2. 登录窗：任务栏应为闪电 logo ✅
3. 登录成功跳主窗：任务栏应**保持闪电 logo**（修复前为空白方块）

## 6. 坑位沉淀

| # | 坑 | 教训 |
|---|---|---|
| 1 | `setWindowFlags` 重建 HWND 丢 HICON，且 QMainWindow+setMenuWidget 路径 Qt 5.15 不自动写回 | Frameless 封装须在**最后一次** setWindowFlags 之后显式 `setWindowIcon`；icon 需调用方传入（重建瞬间读 `windowIcon()` 为空） |
| 2 | 测试用例把**栈对象** `setParent` 给另一栈对象 → 作用域结束 parent 先析构并 delete 子对象 → 子对象栈析构 **double free SIGSEGV**（实测定在构造行） | 生命周期归 QObject 树管理的对象一律堆分配（`new` + parent 链），栈对象只做无 parent 的独立窗口 |
| 3 | offscreen 平台无法直接读 Windows HICON | 用 `windowHandle()->icon()` 观测 Qt 写回 native 层的状态，作为可自动验证的守卫点 |
| 4 | ctest `--preset` 偶发 Invalid preset（dev shell 重置 cwd） | 改用 `ctest --test-dir <buildDir> --output-on-failure`，绕开 preset 文件定位 |
| 5 | LNK1168：手动运行的 ens_app.exe（PID 11488，422 MB）占用 exe 导致重链失败 | `tasklist` 找 PID → `MSYS_NO_PATHCONV=1 taskkill /F /PID` → 复查无残留再链（红线流程复现） |
