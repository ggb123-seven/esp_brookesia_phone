# ESP-IDF Windows 构建环境固定方案

这份说明用于避免 VS Code、CMake Tools、ESP-IDF 插件、命令行混用导致的构建缓存不兼容问题。

## 这次错误的根因

错误信息：

```text
CMake Error: Error: generator : Ninja
Does not match the generator used previously: NMake Makefiles
Either remove the CMakeCache.txt file and CMakeFiles directory or choose a different binary directory.
```

含义：

```text
同一个 build 目录以前用 NMake Makefiles 配置过
现在 ESP-IDF 插件要用 Ninja 配置
CMake 不允许同一个 build 目录混用 generator
```

这不是业务代码错误，而是构建目录缓存里记录的 CMake generator 和当前工具入口不一致。

## 固定原则

这个项目统一按下面规则使用：

```text
构建入口：ESP-IDF 插件或 ESP-IDF PowerShell
目标芯片：esp32p4
CMake generator：Ninja
不要使用：VS Code CMake Tools 的 Configure/Build
```

项目级 `.vscode/settings.json` 已经加入：

```json
"cmake.configureOnOpen": false,
"cmake.configureOnEdit": false,
"cmake.generator": "Ninja",
"C_Cpp.intelliSenseEngine": "disabled"
```

作用：

- 避免 CMake Tools 打开工程时自动用 NMake/Visual Studio Kit 配置。
- 固定 generator 为 Ninja。
- 避免 Microsoft C/C++ IntelliSense 和 clangd 同时抢 ESP-IDF 工程索引。

项目级 `.vscode/extensions.json` 已经推荐：

```text
espressif.esp-idf-extension
llvm-vs-code-extensions.vscode-clangd
```

并把 `ms-vscode.cmake-tools` 标为不推荐。

## 日常正确操作

优先用 VS Code 左侧 ESP-IDF 插件提供的命令：

```text
Set Espressif Device Target
SDK Configuration Editor
Build
Flash
Monitor
```

如果用命令行，进入 ESP-IDF PowerShell 后执行：

```bash
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

## 一键清理缓存

如果又出现类似不兼容问题，运行：

```powershell
.\tools\reset_esp_idf_build.ps1
```

这会删除当前项目的 `build/` 目录。它只删除构建缓存和产物，不删除源码。

如果想清理后立刻重新配置：

```powershell
.\tools\reset_esp_idf_build.ps1 -Reconfigure
```

注意：`-Reconfigure` 需要在已经激活 ESP-IDF 环境的终端里运行，否则找不到 `idf.py`。

## 仍然可能出问题的情况

### ESP-IDF 版本混用

这个项目当前 VS Code 配置指向：

```text
F:\ESP32\ESP-IDF-v5..5.4\v5.5.4\esp-idf
```

不要一会儿用这个路径，一会儿又用 `C:\Espressif\frameworks\...` 或别的 IDF 版本。

### Python 环境混用

ESP-IDF 插件会使用自己的 Python venv。不要手动换成系统 Python 来跑 `idf.py`。

### 目标芯片缓存错误

如果曾经把工程配置成其他芯片，执行：

```bash
idf.py set-target esp32p4
```

这会重新生成与目标芯片相关的配置。

### CMake Tools 仍然自动弹出

如果 VS Code 仍提示 CMake Tools 要 Configure：

1. 不要点击它的 Configure。
2. 在本工作区禁用 CMake Tools 插件。
3. 只使用 ESP-IDF 插件。

## 推荐恢复顺序

遇到构建/配置不兼容时，按这个顺序处理：

```powershell
.\tools\reset_esp_idf_build.ps1
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

如果是在 VS Code 里，清理后重新点击 ESP-IDF 插件的 Build 或 SDK Configuration Editor。
