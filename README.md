# Taskbar Tuner（任务栏调节器）2.0

独立的 Windows 11 任务栏外观调节工具：托盘图标 + 设置面板，单文件 exe，**不需要安装 Windhawk**。

> 调整任务栏背景与图标文字的不透明度，并可调暗任务栏背景，定制出简洁漂亮的任务栏——与桌面融为一体的任务栏，也更护眼、更适合 OLED 显示器。

它直接改写任务栏的 XAML 视觉树和合成层，所以不需要改主题、不需要重启 explorer，拖动滑杆时任务栏立刻变化。

---

## 来源与致谢

核心逻辑移植自 Windhawk 官方模块库里的
`mods/taskbar-brightness-and-opacity-tuner.wh.cpp`（作者 **lzxujun**，GPL-3.0，1,946 行），
亮度与不透明度对任务栏背景的作用方式与该模块 1.13.0 保持一致，因此额外获得：

- v1.12.0 的修复：不再把整层透明度写到包含拖拽把手的元素上（`GripperControl` 单独处理，把手不会浮在任务栏上面）
- 弹出层（悬停预览、托盘溢出菜单、Alt+Tab）被排除在调整范围外，不会在任务栏上方留下一条灰边
- 图标层只在任务栏可见区域内生效，XAML 岛比任务栏高也不会溢出

"显示所有托盘图标"来自 m417z 的 `taskbar-notification-icons-show-all`。

---

## 功能

任务栏被分成两个独立的层，各自用一个简单的 0–100 标尺调整：

- **任务栏背景** —— 图标后面的那块材质。0 = 全透明，100 = 不变。
- **图标和文字** —— 任务按钮、标签和系统托盘。0 = 全透明，100 = 不变。

背景还可以在 0–100 的亮度标尺上**调暗**：0 = 调至全黑，100 = 不变。调暗作用的正是任务栏背景本身显示的内容，绝不是画在它上面的一层覆盖——低而暗的任务栏在夜间更护眼，也能减缓 OLED 烧屏。**只调暗，不调亮。**

此外：

- **显示任务栏顶部分隔线** —— 显示或隐藏任务栏顶缘的细线。
- **显示所有托盘图标** —— 关闭托盘图标折叠，让全部图标直接显示在任务栏上。
- **随 Windows 开机自动启动** —— 以托盘方式静默启动。

关闭程序或恢复默认时会**精确还原**：改过什么就还原什么，用赋值而不是 `ClearValue`（模板元素持有本地值，`ClearValue` 会把填充直接抹掉）。若期间 Windows 自己改过同一个值（切主题、开关透明效果），比对当前值后再决定是否还原，不会用旧值覆盖新值。

---

## 为什么不依赖 PDB

按符号查任务栏内部结构（`TaskbarHost::FrameHeight`、`CTaskBand::GetTaskbarHost` 等）这条路有两个天然的坑：

1. Windows 不自带 `symsrv.dll`，dbghelp 连不上微软符号服务器，只能用磁盘上已有的 PDB；
2. 残留的历史缓存 PDB 即使 GUID/Age 对不上也可能被报告"符号已加载"，随后逐个符号查询失败。

所以这个版本完全不走符号：从任务栏窗口的 user data 拿到 `CTaskBand` 对象，逐个指针槽位扫描找 `TaskbarHost`，再按一组候选偏移取元素指针，**每一个候选项都必须通过"QueryInterface 成 FrameworkElement → 有 XamlRoot → 树里能找到 BackgroundFill"的验证**才算数。验证不过就换下一个候选，不会误认，也不会调用到野指针（QI 包在 SEH 里，出错即返回失败）。

第一次探测成功后缓存布局（槽位 + 偏移），后续每次应用只要两次指针读取；布局失效会自动重扫。

判定"这个指针是不是 XAML 对象"时，模块是按文件名（含 `xaml`）**逐次学习**的，而不是启动时一次性解析：任务栏的 XAML 是延迟加载的，如果注入早于它出现，一次性解析会记下一张空表，之后整个 explorer 进程内所有候选都会被否决（表现为"有时一次就中、有时怎么都不生效"，重启 explorer 才恢复）。现在这张表按需增长，永不锁死。

---

## 目录结构

```
TaskbarTuner/
├─ CMakeLists.txt              两个目标：host 可执行文件 + hook DLL
├─ build.cmd                   一键构建（自动找 VS 工具链）
├─ res/
│  ├─ TaskbarTuner.rc.in       资源脚本模板（图标、内嵌 DLL、清单、版本信息）
│  ├─ TaskbarTuner.ico         程序/托盘图标
│  ├─ app.manifest             PerMonitorV2 DPI、Common Controls v6
│  ├─ resource.h               IDR_HOOK_DLL / IDI_APP_ICON
│  └─ version.h                唯一的版本号来源
├─ src/
│  ├─ shared/                  两边共用的配置与日志
│  │  ├─ Config.h/.cpp         注册表读写、PushConfig、托盘图标开关、开机自启
│  │  └─ Log.h/.cpp            同一份日志文件，host 与 hook 各写各的行
│  ├─ hook/                    注入 explorer.exe 的 DLL
│  │  ├─ dllmain.cpp           隐藏窗口、轮询、重试、消息处理
│  │  ├─ TaskbarHost.cpp       免符号地定位 XamlRoot（上面那一节）
│  │  ├─ VisualTree.cpp        视觉树调整与精确还原
│  │  └─ BackdropBrush.cpp     ColorMatrix 合成特效（三档调暗策略的最后一档）
│  └─ host/                    用户看到的程序
│     ├─ main.cpp              托盘、菜单、单实例、生命周期
│     ├─ Panel.cpp             设置面板（滑杆/复选框用系统控件，背景与按钮自绘）
│     └─ Injector.cpp          释放内嵌 DLL、远程线程注入、等待就绪
└─ tools/
   ├─ make_icon.py            重新生成图标（纯 Python，无依赖）
   └─ build_cl.py             不依赖 CMake 的构建脚本（build.cmd 的退路）
```

构建产物：`build/bin/TaskbarTuner.exe`（单文件，DLL 以 RCDATA 资源嵌在里面，启动时释放到 `%LOCALAPPDATA%\TaskbarTuner`）。

---

## 构建

需要 **Visual Studio 2019/2022**（"使用 C++ 的桌面开发"工作负载，含 Windows SDK）。

```
build.cmd
```

脚本会：用 `vswhere` 找工具链 → 进 MSVC 环境 → 有 CMake 就用 CMake 编译；**没有 CMake 则退回 `tools/build_cl.py` 直接驱动 `cl.exe`**（只需要 Python）。

手工构建，两种方式等价：

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

```
python tools/build_cl.py --vc "%VCToolsInstallDir%." --sdk "%WindowsSdkDir%"
```

`build_cl.py` 的 `--sdk` 既接受已安装的 Windows SDK 目录（含 `Include\`、`Lib\`），也接受 `xwin splat` 产出的扁平布局；`rc.exe` 未指定时会自动在 SDK 里找。

注意：hook DLL 必须先编译出来，因为它的二进制会被作为资源编译进 exe（`add_dependencies` 已保证顺序）。

---

## 亮度到底改了什么

**完全照 Windhawk 模块 `taskbar-brightness-and-opacity-tuner` 的 `ApplyBackgroundStyle` 实现**，没有自行发挥。亮度和不透明度是两个独立的动作：

- **不透明度**：`backgroundFill.Opacity(opacity / 100.0)`
- **亮度**：`scale = brightness / 100`（窗口里 100 = 不变，0 = 全黑），然后按顺序尝试三条策略，命中即返回：
  1. 填充是 `AcrylicBrush` → **只缩放 `TintColor` 的 RGB，保留 alpha**。画笔本身不替换，DWM 材质层原样保留
  2. 填充是 `SolidColorBrush` → 新建同色压暗的画刷写回 `Fill()`，复制原 `Opacity`
  3. 都不是 → 用 `BackdropAdjustBrush` 替换 `Fill()`：取 `CreateBackdropBrush()` 过一遍 ColorMatrix（对角缩放，bias 全 0，`ClampOutput = true`）

`TintLuminosityOpacity` 和 `FallbackColor` **不写**，这是原模块的行为：它只把 tint 当作压暗的杠杆。

日志会明确打印命中了哪一条：

```
background fill: Windows.UI.Xaml.Media.AcrylicBrush tint=FF202020 tintOpacity=0.50 lumiOpacity=0.96 fallback=FF1C1C1C (element opacity=0.80, visible=1)
acrylic tint dimmed FF202020 -> FF0D0D0D (scale=0.40)
```

`scale=0.40` 就是亮度 40%。若换机器后看不到变化，看这两行：tint 权重低的机器（`lumiOpacity` 接近 1 时）tint 压暗的视觉幅度确实有限——这是原模块策略本身的表现，不是这里改坏了。

还原同样按原模块做所有权比对：只有当前 tint 仍等于我们写进去的值才恢复，避免覆盖 Windows 后来改的结果；替换过 `Fill()` 的用弱引用比对是不是我们装的那支笔刷。

---

## 怎么确认它真的生效

最可靠的证据是日志，而不是截屏像素：

- `applied to 0000000000230496` —— 配置已写进任务栏
- `foreground elements adjusted: 6` —— 图标/文字层处理了 6 个元素
- `acrylic tint dimmed FF202020 -> FF0D0D0D (scale=0.40)` —— 亮度 40% 已作用在 tint 上
- 退出时 `restored` —— 精确还原

`tools/verify_taskbar.py` 也保留了，它会逐档设置参数并用 `PrintWindow(PW_RENDERFULLCONTENT)` 取任务栏像素对比。**但这台机器上它只能当参考**：任务栏在独立合成平面上，`BitBlt` 抓不到；`PrintWindow` 在 acrylic 参数刚被改动的一小段时间里会返回全黑或过期帧，基线又会随壁纸和任务栏内容变化而漂移，所以数值波动可能很大。判断功能请以日志命中的策略为准。

备注：亮度档位在窗口里是 0..100 的连续值（左边 0、右边 100），不吸附整数步进。

全部回到中性值时会**精确还原**：改过什么就还原什么，用赋值而不是 `ClearValue`（模板元素持有本地值，`ClearValue` 会把填充直接抹掉）。若期间 Windows 自己改过同一个值（切主题、开关透明效果），比对当前值后再决定是否还原，不会用旧值覆盖新值。

---

## 使用

1. 运行 `TaskbarTuner.exe`，面板自动弹出，托盘出现图标。
2. 拖滑杆，任务栏实时变化；松手才写入注册表并把结果推给 hook（拖拽过程不会每帧写盘）。
3. 左键托盘图标 = 开关面板，右键 = 菜单（启用/暂停、恢复默认外观、开机自启、关于、退出）。

| 控件 | 作用 | 中性值 |
| --- | --- | --- |
| 亮度 | 背景亮度，0..100，0 = 全黑；按原模块策略缩放 acrylic 的 `TintColor` | 100% = 不变 |
| 不透明度（任务栏背景） | 背景元素 `Opacity` | 100% = 不变 |
| 不透明度（任务按钮和系统托盘） | 图标与文字层的 `Opacity`（乘法叠加，系统隐藏的元素保持隐藏） | 100% = 不变 |
| 显示任务栏顶部分隔线 | `BackgroundStroke` 的可见性（只改 Opacity，不动 Fill/Stroke，避免破坏模板） | 开 |
| 显示所有托盘图标 | 批量把 `NotifyIconSettings\*\IsPromoted` 置 1，并备份原值 | 关 |
| 随 Windows 开机自动启动 | `Run` 项里写 `"<路径>" --tray` | 关 |

---

## 配置与日志

- 配置：`HKCU\Software\TaskbarTuner`
  `Opacity` / `Brightness` / `IconOpacity` / `TopLine` / `HideGrip` / `ShowAllTray` / `Enabled` / `CfgVersion`

  亮度按**窗口显示的范围**存（`0..100`，`100` = 不变，`0` = 全黑）。
- 托盘图标备份：`HKCU\Software\TaskbarTuner\TrayIconsBackup`
- 日志：`%LOCALAPPDATA%\TaskbarTuner\TaskbarTuner.log`（超过 1 MB 自动轮转）
  host 与 hook 写同一个文件，注入失败时两边的记录会挨在一起。

---

## 故障排查

**面板显示"未连接到任务栏"**
注入没成功。日志里会写明是哪一步：找不到任务栏窗口 / 打不开 explorer 进程 / 创建线程失败。
常见原因：安全软件拦 `CreateRemoteThread`；程序与 explorer 权限不一致（这种情况请用同一账户、同样的完整性级别运行）。

**注入成功但任务栏没变化**
看日志里有没有 `no taskbar host found` 或 `taskbar host found: slot 0x.. offset 0x..`：
- 前者说明这台机器的任务栏布局在这套候选偏移里没匹配上，属于需要补偏移的情况；
- 后者加上 `icon and text layer is empty` 说明任务栏还没布局完，程序会在 1.2 秒后重试（连续失败 5 次后转为 30 秒一次，不再刷屏）。

**explorer 重启后效果消失**
程序在收到 `TaskbarCreated` 消息后会重新注入并推一次配置，等一两秒即可；托盘图标也会自动重新注册。

**想彻底停掉**
右键托盘 → 退出。退出前会把任务栏还原、把托盘图标恢复原状，并**卸载注入到 explorer 里的 DLL**（不卸载的话它会一直锁住 `%LOCALAPPDATA%\TaskbarTuner` 下的文件，想删都删不掉）。配置值保留，下次启动恢复同样的外观。

---

## 已知限制

- 仅 Windows 11（依赖 WinUI 任务栏的 XAML 结构，`BackgroundFill`/`BackgroundStroke` 这些元素名）
- 仅 x64
- 任务栏对象布局靠"候选偏移 + 严格验证"定位，微软大改内部结构时需要补一组候选偏移（改 `kElementOffsets` 一行）
- 需要与 explorer 相同或更高的完整性级别

---

## 许可

GPL-3.0。核心视觉树逻辑来自 Windhawk 模块 `taskbar-brightness-and-opacity-tuner`（作者 lzxujun，GPL-3.0），
"显示所有托盘图标"逻辑来自 `taskbar-notification-icons-show-all`（作者 m417z）。
