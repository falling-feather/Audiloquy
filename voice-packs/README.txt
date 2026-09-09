Audiloquy / 语澜本地声音包协议（schema 1）
===========================================

基础发行包不会内置来源或再分发许可尚未核清的神经 TTS 运行时与模型。
把经过自行核验的声音包清单放在：

  %LOCALAPPDATA%\Audiloquy\voice-packs\*.voice-pack.json

也可在 Audiloquy.exe 同目录创建 voice-packs 文件夹。清单中的 helper 必须是声音包
目录内部的相对路径，防止清单越界执行其他程序。

最小清单：

{
  "schemaVersion": 1,
  "id": "my-approved-pack",
  "name": "My approved local voices",
  "helper": "bin/audiloquy-tts-helper.exe",
  "distributionMode": "user-supplied",
  "runtimeLicense": "请填写运行时许可证",
  "modelLicense": "请填写模型许可证",
  "voices": [
    {"id":"us-female","name":"US Female","locale":"en-US","gender":"female"},
    {"id":"us-male","name":"US Male","locale":"en-US","gender":"male"},
    {"id":"gb-female","name":"GB Female","locale":"en-GB","gender":"female"},
    {"id":"gb-male","name":"GB Male","locale":"en-GB","gender":"male"}
  ]
}

helper 调用契约：

  helper --manifest <清单> --voice <voice-id> --locale <en-US|en-GB>
         --wpm <整数> --text-file <UTF-8 文本> --output-wav <临时 WAV>

退出码必须为 0；输出必须为 PCM16、单声道、44,100 Hz WAV。应用会在采用文件前
重新解析并校验格式。声音包负责模型加载、音色真实性说明、WPM 校准和模型许可。
# Kokoro 英语声音包（2026-09-09）

应用内“安装神经音色”会运行 scripts/install-kokoro.ps1，固定下载并校验：
- sherpa-onnx v1.13.7 Windows x64 shared MT Release（独立 CLI 程序）
- Kokoro int8 multilingual v1.0（本包仅开放英语）

默认位置：%LOCALAPPDATA%/Audiloquy/voice-packs/kokoro-en/
根目录清单：kokoro-en.voice-pack.json
声音：af_heart=3（美音女）、am_michael=16（美音男）、bf_emma=21（英音女）、bm_george=26（英音男）。

模型来源与ID表：https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/kokoro.html
运行时：https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.7
主程序不链接该引擎；独立 C++ helper 用命令行/WAV 与上游程序通信，Windows Job Object 保证取消 helper 时终止引擎。
模型与运行时从上游直接下载，基础发行包不包含其二进制/权重；第三方许可在安装目录和 upstream source 中保留。

引擎原生24kHz，helper转换为44.1kHz PCM16单声道；WPM由各声音基准校准，应用显示实测值。
英文智能引号/破折号会规范化，拉丁名字使用分解后的基本字母。中文等非英语符号会明确拒绝，不生成乱码语音。
首次需网络，安装完成后不上传文稿、可离线生成。高级离线安装可给脚本传 -CacheDirectory 指向两份官方压缩包；仍验证固定SHA-256。
