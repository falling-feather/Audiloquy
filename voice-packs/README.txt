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
