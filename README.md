# MeetSchedule Desktop Widget

轻量的 Windows 原生课程桌面组件，使用 C++17、Win32 和 GDI+，通过 ICS 订阅显示当前或下一节课程。无需 Python、浏览器或额外 C++ 运行库。

```text
离下课 1:10:00
科技英语阅读与写作
5401 · 王老师
```

## 功能

- 透明背景、微软雅黑白字、暗色文字阴影。
- 严格三行：倒计时、课程名、地点和教师。
- 每行根据内容和可用空间自动选择字号，尽量撑满区域；课程名和倒计时加粗。
- 倒计时每秒更新，跨越上下课时间时自动切换课程。
- 每 5 分钟在后台重新拉取 ICS，并将课程数据保存为本地 JSON。
- 优先挂载桌面层，普通应用可以覆盖组件；不抢焦点、不显示任务栏按钮。
- 托盘右键支持立即刷新、查看同步状态、退出；重复启动不会创建多个实例。

## 运行

支持 Windows 10/11 x64。将以下文件放到同一目录：

```text
MeetScheduleWidget.exe
meetics.txt
THIRD_PARTY_NOTICES.txt
```

在 exe 同目录创建 UTF-8 编码的 `meetics.txt`，内容只填写一行完整的 HTTPS 日历订阅链接（不加引号）。随后双击 `MeetScheduleWidget.exe` 启动，无命令行参数。文件不存在、无法读取、为空或链接格式无效时，会弹窗提示并退出；修正后重新启动。退出程序请右键托盘图标选择“退出”。替换新版 exe 前先退出旧程序。

组件不会自动注册开机启动。复制 exe 即可部署，课程缓存保存在当前 Windows 用户的数据目录。

## 位置与字体

以**主显示器完整桌面**为基准，包括任务栏所在区域。宽高分别八等分成 8×8 网格，组件占从左上角数的**第 7 行、第 7 列**，即沿主对角线从右下角倒数第二格。

| 项目 | 默认值 |
| --- | --- |
| 宽、高 | 各占桌面 12.5% |
| 横向范围 | 75%–87.5% |
| 纵向范围 | 75%–87.5% |
| 中心位置 | (81.25%, 81.25%) |
| 字体 | 微软雅黑，每行动态适配 |

1920×1080 屏幕上的组件约为 240×135 像素，取整可能产生 1 像素偏差。修改 `main.cpp` 顶部的 `WidthRatio`、`HeightRatio`、`CenterX`、`CenterY` 后重新编译即可调整大小与位置。字体测量结果会缓存，同位数倒计时更新时保持字号稳定。

## 订阅与缓存

订阅地址在启动时从 **exe 同目录的 `meetics.txt`** 读取，与当前工作目录无关。更换链接只需修改此文件并重启程序，无需重新编译。支持 UTF-8（可带 BOM）以及文件首尾的空白和换行。缓存中的来源地址与新地址不一致时，会自动重新拉取。

只读取 `CATEGORIES:课程` 的事件，跳过已取消事件，保留自修课程。服务端已将单双周、调课等展开成独立事件，组件不重复计算这些规则。当前解析器支持 ICS 折行、文本转义、UTC 以及上海时区，不支持 RRULE/RDATE 等重复规则或任意时区；全天事件不参与课程倒计时。

内部使用 Unix 时间戳。上海墙上时间按 UTC+8 换算，不受 Windows 当前时区影响。网络请求通过系统 WinHTTP 完成，保持 HTTPS 证书校验开启。

缓存位于：

```text
%LOCALAPPDATA%\MeetScheduleWidget\course_cache.json
```

缓存有效期为 300 秒，由 `course_data.hpp` 中的 `CacheSeconds` 控制。JSON 包含版本、来源地址、获取时间、过期时间和课程列表；时间字段使用 Unix 秒时间戳。

启动时优先使用尚未过期的缓存；运行期间每次同步成功后，间隔 5 分钟重新请求 ICS。失败时每分钟重试，有旧缓存则继续显示旧数据，并在托盘“同步状态”中标记过期。首次启动无缓存且请求失败时，组件显示网络错误提示。网络请求在后台线程执行，不阻塞倒计时绘制。

`meetics.txt` 和课程缓存包含私人数据，已加入 `.gitignore`。源码和 exe 不再内置个人订阅地址；分享程序时不要附带自己的 `meetics.txt` 或缓存。

## 编译

### macOS 交叉编译 Windows x64

安装 Homebrew 和 Xcode Command Line Tools 后，在项目根目录运行：

```sh
brew install mingw-w64
sh build-macos.sh
```

输出为 `dist/MeetScheduleWidget.exe` 和 `dist/THIRD_PARTY_NOTICES.txt`；项目根目录存在 `meetics.txt` 时会一并复制到 `dist`。脚本静态链接 GCC/C++ 运行库，程序运行时仅依赖 Windows 自带 DLL。macOS 可以编译 exe，但不能直接原生运行它。

### Windows 编译

安装 Visual Studio 的“使用 C++ 的桌面开发”组件，在 **x64 Native Tools Command Prompt** 中进入项目目录并执行：

```bat
build-windows.cmd
```

输出同样位于 `dist`，使用 `/MT` 静态链接 MSVC 运行库；根目录的 `meetics.txt` 存在时会一并复制。

## 源码结构

```text
main.cpp             链接文件读取、窗口、动态排版、托盘、下载与缓存
course_data.hpp      ICS 解析、课程选择和 JSON 数据结构
build-macos.sh       macOS → Windows x64 编译脚本
build-windows.cmd    Windows MSVC 编译脚本
vendor/json.hpp      nlohmann/json 3.12.0 单头文件库
vendor/LICENSE.json  nlohmann/json 的 MIT 许可证
```

`.gitignore` 排除系统文件、构建产物、Visual Studio 本地状态、订阅链接文件和课程缓存；源码、编译脚本及第三方许可证保留在版本控制中。

## 验证范围与桌面兼容性

已验证 macOS 交叉编译、真实 ICS 解析、缓存 JSON 往返及 exe 动态依赖。窗口、字体和桌面层级尚未在 Windows 实机确认。

桌面挂载使用 Explorer 的 WorkerW，这是未公开的实现细节，可能受到 Windows 版本或第三方桌面软件影响。无法找到桌面宿主时会使用普通置底窗口，此模式下“显示桌面”的行为可能不同。重启 Explorer 后如果组件消失，重新运行 exe。可通过托盘“同步状态”查看当前挂载模式和网络错误。

## 代码格式

自有 C++ 源码使用 `.clang-format`，Shell 脚本使用两空格缩进。第三方 `vendor` 目录保留上游格式。

```sh
clang-format -i main.cpp course_data.hpp
shfmt -w -i 2 -ci -sr build-macos.sh
```

检查格式和 Shell 语法：

```sh
clang-format --dry-run --Werror main.cpp course_data.hpp
shfmt -d -i 2 -ci -sr build-macos.sh
sh -n build-macos.sh
```
