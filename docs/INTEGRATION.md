# 整合记录

## 来源与版本选择

| 原内容 | 新位置 | 处理方式 |
| --- | --- | --- |
| `ESP32/10` | `firmware/esp32s3` | 保留主控及 BSP 源码、分区表和原硬件配置 |
| `ESP32/bule teeh1` | `firmware/esp32-a2dp` | 保留蓝牙源码、连接提示 PCM 和示例教程 |
| 桌面 `work/elysia.py` | `backend/elysia.py` | 合并为同一份服务端 |
| `elysia-ubuntu-deploy-20260917.before-tcp-timeout.zip` | `backend/`、`deployment/ubuntu/` | 复用跨平台路径逻辑、角色包和 systemd 服务 |
| `零件/外壳.STL` | `hardware/3d/enclosure.stl` | 原字节保留，发布文件名统一为英文 |
| `lceda-pro/record me.eprj2` | `hardware/pcb/source/record-me.eprj2` | SQLite 一致快照，清除公开副本中的账号字段，设计及历史保持不变 |
| `lceda-pro/record me_backup` 中最新 `.epro2` | `hardware/pcb/source/record-me-backup-20260821-1127.epro2` | 保留已有便携备份，明确备份时间 |

比较结果：桌面脚本与指定 ZIP 内的服务端仅有缓存目录、声纹模型目录和输出目录的部署路径差异。整合以指定 ZIP 的跨平台实现为基础，保留桌面脚本中的对话与音频处理函数。没有引入工作目录中其他时间点的服务端版本，也没有使用名字不含 `before-tcp-timeout` 的另一份部署 ZIP。

所有输入均以只读方式使用，原文件未被修改。两套固件关键输入的 SHA-256 留在相邻的 `source-*.sha256` 文件中。新增硬件资料的校验值见 `hardware/asset-manifest.json`。

## 调整范围

- 服务端使用项目内默认缓存与模型路径，并允许环境变量覆盖；自动读取本地 `backend/.env`，外部环境变量优先。
- 监听地址、端口和声纹开关可通过环境变量设置，原默认行为保持：5000 端口，声纹启用。
- 服务端处理 SIGTERM，配合 Ubuntu systemd 正常退出。
- Wi-Fi、服务器地址从公开源码中的具体值改为本地配置头文件；示例不含真实网络信息。
- 蓝牙目标地址改为本地配置头文件；未配置地址时使用源码已有的按设备名称发现流程。
- 主控 BSP 明确列出源文件，避免同时编译两个包含同名函数的 TCP 实现。`tcp_stream_3s.c` 作为替代实现保存在 `extras/`，不参与默认构建。
- 主控 CMake 移除了原来引用但不存在的 `components/Middlewares` 路径；两个项目使用清晰、无空格的项目名。
- `sdkconfig.defaults` 保留原实际配置，包括关闭的选项；蓝牙设备名称替换为通用示例，运行时生成的 `sdkconfig` 忽略。
- 使用提供二进制 wheel 的 `webrtcvad-wheels`，Python 导入名仍为 `webrtcvad`；ModelScope 安装框架依赖集合，补齐 pipeline 需要的基础库。PyTorch 与 torchaudio 单独安装。
- 个人声纹录音、模型权重、旧 Git 历史、缓存、编译输出和临时备份不进入上传包。必需的连接提示 `connected.pcm` 保留。

## 验证

已完成 Python 语法、JSON、角色包完整性检查；原 Wi-Fi、服务器地址和蓝牙地址的泄露扫描未发现残留；Git 忽略规则覆盖密钥、本地配置、声纹录音、模型和构建文件。

模型准备脚本已从指定旧 ZIP 恢复两个模型文件，并验证 SHA-256 与原文件一致，没有提取个人声纹录音。在线模型下载需要网络和 ModelScope，未在本次整理中执行。

两套固件均已使用本机 ESP-IDF 5.5.5 完整编译、链接、生成映像，并通过分区大小检查：

| 固件 | 目标 | 应用映像大小 | 应用分区大小 | 结果 |
| --- | --- | --- | --- | --- |
| 主控 | esp32s3 | `0xcaa30` 字节 | `0x1f0000` 字节 | 通过 |
| 蓝牙端 | esp32 | `0x138c20` 字节 | `0x177000` 字节 | 通过 |

构建使用原配置的复制件和公开示例连接参数，在较短的纯英文临时路径完成。原中文工作目录触发了工具链 objdump 路径兼容问题，较长的路径另触发了 Windows 文件长度限制；README 已注明建议路径。构建产物不放入源码上传包。

未在新虚拟环境中安装全部 Python 依赖、连接实体设备、调用收费云端 API 或执行 Ubuntu 部署；完整语音链路需要配置自己的账号和硬件后验证。

新增硬件资料：STL 通过长度、面数、坐标及几何边共享检查；PCB 快照通过 SQLite 完整性及设计数据一致性检查；便携备份通过 CRC 与原文件 SHA-256 一致性检查。原理图、PCB 预览取自当前工程内置图片。没有重新构建固件，代码未因添加硬件资料而变化。
