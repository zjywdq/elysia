# 第三方来源说明

- 蓝牙固件保留 Espressif 示例文件中的 SPDX 声明：`Unlicense OR CC0-1.0`。来源为 [ESP-IDF A2DP Source 示例](https://github.com/espressif/esp-idf/tree/v5.5.5/examples/bluetooth/bluedroid/classic_bt/a2dp_source)。原目录中的教程随附于 `firmware/esp32-a2dp/tutorial/`。
- CAM++ 模型通过 [ModelScope](https://www.modelscope.cn/models/iic/speech_campplus_sv_zh-cn_16k-common) 单独获取，不随本仓库分发模型权重。
- ModelScope 的基础 pipeline 依赖使用其 [framework 依赖集合](https://github.com/modelscope/modelscope/blob/master/requirements/framework.txt)。
- WebRTC VAD 使用 [webrtcvad-wheels](https://github.com/daanzu/py-webrtcvad-wheels)，服务端仍使用 `import webrtcvad`。
- Qwen-ASR、CosyVoice 与 GLM 通过各自的云端 SDK 和用户账号访问。
- 角色包沿用原部署包的文本和示例，涉及《崩坏3》的角色背景；相关角色及品牌权利归其权利人。
- `connected.pcm` 为原蓝牙工程用于连接提示的嵌入音频，编译时需要。原项目未提供其独立授权说明，发布者应按自己持有的权利决定保留或替换。

本次整理没有对整个项目新增统一许可证，也没有更改第三方文件原有的许可声明。
