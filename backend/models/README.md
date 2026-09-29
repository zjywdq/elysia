# CAM++ 模型

需要声纹验证时，准备下面两个文件：

```text
models/CAM++/configuration.json
models/CAM++/campplus_cn_common.bin
```

本次整合未将约 28 MB 的权重或个人 `enroll_0.wav` 放进 Git 仓库。已有部署 ZIP 可以使用 `scripts/prepare_model.py --archive <ZIP路径>` 恢复其中的两个模型文件，不会恢复个人声纹录音。

没有本地模型时，使用 `scripts/prepare_model.py --download` 下载 [ModelScope 的 CAM++ 模型](https://www.modelscope.cn/models/iic/speech_campplus_sv_zh-cn_16k-common)。模型库编号为 `iic/speech_campplus_sv_zh-cn_16k-common`；模型许可和使用条件以来源页面为准。

准备脚本默认输出到此目录下的 `CAM++` 子目录；也可用 `--target` 指定目录，并在服务端 `.env` 中设置 `SV_MODEL_PATH` 为该目录的绝对路径。未准备模型时，可将 `SPEAKER_VERIFICATION_ENABLED=false` 暂时关闭声纹功能。
