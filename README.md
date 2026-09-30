# RunMeCpp

RunMe 启动器的 **C++/Win32 单文件实现**（对照上级目录的 C# 版 `..\RunMe` 移植，行为语义一致）。

编译产物为无运行时依赖的独立 exe（x64、`/MT` 静态链接、Unicode）。

> 与 C# 版的差异：列表窗口的“倒计时自动启动”与“方向键循环切换”为 C++ 版新增，C# 版无此行为；其余语义一致。

## 构建

- Visual Studio：打开 `RunMeCpp.sln`，选 **Debug/Release × x64/Win32** 任一组合生成
- 命令行：

```
msbuild RunMeCpp.sln /p:Configuration=Release /p:Platform=x64     # 64 位
msbuild RunMeCpp.sln /p:Configuration=Release /p:Platform=Win32   # 32 位
```

产物：

| 平台 | Debug | Release |
|---|---|---|
| x64 | `bin\Debug\RunMeCpp.exe` | `bin\Release\RunMeCpp.exe` |
| Win32 (x86) | `bin\Win32\Debug\RunMeCpp.exe` | `bin\Win32\Release\RunMeCpp.exe` |

两个平台行为一致（两套测试在 x64 与 Win32 上均全过）；x86 版给 32 位 Windows 生产机用。

exe 带版本资源：右键「属性 → 详细信息」可直接看到 **64 位 x64 / 32 位 x86**、版本号与版权（版本号在 `version.h` 维护，发版时与 git tag 对齐）。

## 功能（与 C# 版一致）

- **改名即用**：exe 改名成入口名（如 `Vs.exe`），在 `YanBinCfg.ini` 的 `[Config]` 配同名键
- **首启生成配置**：首次运行自动生成带注释的 `YanBinCfg.ini`（UTF-8 with BOM）
- **执行标记**（顺序任意，可组合）：`cmd` / `ps`(`powershell`) / `runadmin`（管理员，弹 UAC）/ `show`（显示窗口，默认隐藏）
- **占位符**：`{time.格式}`（.NET 风格常用子集）、`{env.变量名}`、`{guid.id}`、`{random.最小-最大}`，以及 `{0}{1}…` 命令行参数填充
- **路径解析**：绝对路径直接运行；相对路径支持 `pf\`、`pf86\`、`AppData`、`..\` 前缀，其余以 `[Settings] RunParentDirectory` 为基准拼接
- **runme 列表窗口**：值以 `runme ` 开头 + `显示名|目标,…`；单条直接启动，多条弹 Win32 列表窗口
  （Enter 启动 / Shift+Enter 管理员启动 / 双击启动 / 滚轮循环切换 / 方向键循环切换（末项↓回首项、首项↑跳末项）/ Esc 关闭）
- **列表窗口倒计时**：`[Settings] ListAutoRunSeconds` 秒（默认 5）内无操作，则自动启动当前选中项（未操作时即默认第一项）；
  标题栏显示剩余秒数，窗口宽度会按标题自动撑宽；用户一旦按键 / 滚轮 / 点击即取消倒计时，不再自动启动（设 0 = 完全关闭自动启动）
- **批量启动**：`{分身名}run.txt` 每行一个目标，首行立即、之后每行间隔 1 秒依次启动
- **命令行命令**：`help`、`list 扩展名 [目录]`、`runme 显示名|目标,…`、`runmeth`（其它 exe 全部替换为自身）、`runmefth`（按 `[Config]` 键批量生成分身）
  ——后两者执行完会报告「删除 N 个 / 生成 M 个」及失败项原因（在 shell 中运行打印到控制台，双击运行则弹窗）

## 测试

`docs\run-tests.ps1` 为端到端黑盒测试（29 项，含列表窗口 UI、占位符、run.txt、runmeth 等），默认测试 `bin\Release\RunMeCpp.exe`：

```
powershell -NoProfile -ExecutionPolicy Bypass -File docs\run-tests.ps1
# 或指定 exe：  ... -Exe d:\path\to\RunMeCpp.exe
```

当前结果：**29 / 29 通过**（Release x64，零编译警告）。

`docs\run-tests-extra.ps1` 为扩展测试（列表窗口双击/滚轮、列表倒计时自动启动/可关闭/操作取消、方向键循环、窗口宽度自适应、`AppData`/`pf\`/尾斜杠 `..\` 路径前缀、`{time.*}`/`{random.*}` 格式）：

```
powershell -NoProfile -ExecutionPolicy Bypass -File docs\run-tests-extra.ps1
```

当前结果：**13 / 13 通过**。

两个脚本均支持 `-Exe <路径>` 指定被测 exe（如测 32 位版：`-Exe bin\Win32\Release\RunMeCpp.exe`）。

## 注意（cmd 语法）

配置值里占位符紧贴 `>` 时（如 `echo {random.1-99}>a.txt`），cmd 会把“数字>”当成句柄重定向（句 5、句 0…），导致文件为空。
写成 `echo {random.1-99} >a.txt` 或加方括号 `echo [{random.1-99}]>a.txt` 即可。这是 cmd 自身语法，与 C# 版表现一致。

## 文件

- `main.cpp`：全部实现（单文件）
- `version.h` / `version.rc`：版本号与 exe 版本资源（属性面板里的信息、架构标识）
- `RunMeCpp.sln` / `RunMeCpp.vcxproj`：VS 工程（Debug/Release × x64/Win32）
- `docs\run-tests.ps1`：黑盒测试脚本
