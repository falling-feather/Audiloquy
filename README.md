# Audiloquy · 语澜

面向中学英语教师的本地优先听力制作工具：整理文稿、生成语音、按题播放，把一套材料带到教室使用。

当前为 **Windows x64 开发原型**，采用 C++20 与 Qt 6 Widgets。播放器、文稿编辑和本地情景生成无需账号；联网润色由教师主动启用。

![语澜备课界面](doc/image/independent-preparation-20260908.png)

## 已实现功能

- **题组编辑**：从空白开始，新增、复制、删除、拖动排序，支持题组结构撤销和重做。
- **语音制作**：选择英音/美音、男女音色和目标 WPM，按角色逐轮生成 WAV；音色缺失会明确提示。
- **后台生成**：显示进度、允许取消；只改停顿或重复次数时复用语音缓存，取消保留已完成题组。
- **情景初稿**：输入英文题干、A/B/C 选项、答案和主题，本地生成 When / Where / Why / What 类型男女对话；可选 DeepSeek 润色。
- **课堂播放**：生成后自动试听，按题组播放、循环、暂停、继续、拖动定位及整套播放。
- **保存与搬运**：未完成草稿可安全保存，支持异常退出后恢复；将工程和音频打包为可整体搬移的文件夹。

## 开始使用

本仓库同步源码和项目文档。`dist/` 中的本机构建产物不纳入 Git；当前尚未发布 GitHub 二进制 Release。

已有便携版时，请保留完整解压目录，运行其中的 `Audiloquy.exe`，不要单独移动 EXE。

1. 新建工程，填写题组文稿，或选择示例开始。
2. 选择实际可用的音色、口音和目标语速。
3. 设置每组播放遍数和每遍结束后的停顿。
4. 点击“生成并试听选中题组”，确认后生成整套。
5. 保存工程；需要带到教室时点击“打包带走”，复制整个文件夹，并在语澜中打开其中的 `project.json`。

完整操作、文件位置和可选服务配置见 [使用说明](README.txt)。

## 声音与生成内容的边界

- 基础版使用 **Windows SAPI 系统音色**。英美口音及男女声是否可用，取决于电脑实际安装并可被 SAPI 枚举的音色；不保证每台电脑开箱即有四种声音。
- 默认严格匹配口音和角色。可显式允许性别回退，界面会提示实际采用的声音。
- **WPM 是目标值**；当前 SAPI 速率映射是近似值，不是实测语速校准。
- 已提供 [外置本地声音包协议](voice-packs/README.txt)，仓库未附带神经语音运行时或模型。
- 本地文稿生成采用有限模板；难度选项不代表完整 CEFR 分级。生成的是配套练习材料，不保证还原遗失的原始文稿。
- 结构和事实锚点检查不能证明答案语义唯一，教学使用前仍需教师复核。

## 从源码构建

已验证环境为 Windows x64、MSYS2 UCRT64、GCC 15、Qt 6 Widgets/Network、CMake 与 Ninja。无需 Python 或 Node.js 运行软件。

在项目根目录的 PowerShell 中运行：

```powershell
.\scripts\build-prototype.ps1 -ToolchainBin 'D:\msys64\ucrt64\bin'
.\scripts\run-prototype.ps1
```

将工具链路径替换为实际安装位置；脚本也会自动查找 D:/、C:/ 下的 MSYS2，或读取 `AUDILOQUY_UCRT64_BIN`。

构建脚本会编译、运行 CTest 并收集便携运行库。可用 `-SkipPackage` 跳过打包，或用 `-PackageDirectory` 指定干净的打包目录。输出通常位于：

```text
build/prototype/Audiloquy.exe   编译产物
dist/Audiloquy/                含运行库的便携目录
```

## 验证

2026-09-08 的本地验证结果：**17/17 项测试通过**；便携包在仅保留 Windows 系统 PATH 的环境下通过保存、重开、生成、搬移后打开和播放流程。

```powershell
$env:Path = 'D:\msys64\ucrt64\bin;' + $env:Path
ctest --test-dir .\build\prototype --output-on-failure
```

测试包含真实 SAPI 合成与取消，运行需要可用的英语 SAPI 音色及音频设备。常规测试不会调用付费 API；`deepseek_live_smoke` 是另行手动运行的目标。上述结果是本地验证记录，并非云端 CI 或学校实地验收。

## 可选 DeepSeek 配置

默认流程离线运行。需要润色时，可在本机设置 `DEEPSEEK_API_KEY` 环境变量后启动程序，或按 [使用说明](README.txt) 配置本地私有文件。

主动选择 DeepSeek 会发送题干、选项、答案、主题及本地初稿；返回稿仍需校验和人工审阅。密钥不写入工程、日志或仓库，请勿将其提交到 GitHub。

## 项目导航

| 目录 | 内容 |
| --- | --- |
| `src/core/` | 工程模型、情景模板、历史、保存与恢复 |
| `src/app/` | Qt 界面、后台生成、声音包及可选服务接入 |
| `src/audio/` | PCM WAV 编排 |
| `src/platform/windows/` | SAPI 合成与 WinMM 播放 |
| `tests/` | 模块测试和桌面流程冒烟 |
| `examples/` | 自制示例工程 |
| `doc/` | 产品目标、开发手册、规划和历史 |

- [项目总纲](doc/00-项目总纲.md)
- [开发者文档](doc/01-开发者文档.md)
- [后续规划](doc/02-项目规划.md)
- [开发历史](doc/03-开发历史.md)

后续重点：实际可安装的优质英美男女声音包、独立课堂界面、多题共用材料与已有录音题号标记。PDF/OCR 导入、MP3 导出和自动更新尚未实现。

## 权利与反馈

项目尚未指定开源许可证；公开源码不等于授予任意再分发或商业使用授权。第三方依赖说明见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。下载的考试原件、私有配置、生成音频及模型文件不随源码上传；公开二进制分发仍需完善第三方许可材料。

欢迎通过 [Issues](https://github.com/falling-feather/Audiloquy/issues) 提交问题，附上复现步骤、Windows 版本和实际音色名称；提交示例材料前请移除密钥及个人信息。
