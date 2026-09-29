# Ubuntu 部署

以 Ubuntu 22.04 的 Python 3.10 为例，将 `backend/` 的内容安装到 `/opt/elysia`，systemd 配置中的路径与此一致。以下从已克隆的仓库根目录开始。

## 系统依赖及服务目录

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip build-essential libsndfile1 ffmpeg rsync
id elysia >/dev/null 2>&1 || sudo useradd --system --home-dir /opt/elysia --create-home --shell /usr/sbin/nologin elysia
sudo mkdir -p /opt/elysia
sudo rsync -a --exclude='.env' --exclude='__pycache__' --exclude='cache' --exclude='output' --exclude='SpeakerVerification_DIR' backend/ /opt/elysia/
sudo chown -R elysia:elysia /opt/elysia
```

## Python 依赖

```bash
sudo -u elysia python3 -m venv /opt/elysia/.venv
sudo -u elysia /opt/elysia/.venv/bin/python -m pip install --upgrade pip setuptools wheel
sudo -u elysia /opt/elysia/.venv/bin/python -m pip install torch torchaudio --index-url https://download.pytorch.org/whl/cpu
sudo -u elysia /opt/elysia/.venv/bin/python -m pip install -r /opt/elysia/requirements.txt
```

声纹启用时，将 CAM++ 的 `configuration.json` 和 `campplus_cn_common.bin` 放入 `/opt/elysia/models/CAM++`。可先在本机从旧 ZIP 恢复，再上传；也可从仓库根目录执行：

```bash
sudo -u elysia /opt/elysia/.venv/bin/python scripts/prepare_model.py --download --target /opt/elysia/models/CAM++
```

下载脚本所在的仓库路径需要允许服务用户读取。

## 配置与前台验证

```bash
sudo cp /opt/elysia/.env.example /etc/elysia.env
sudo nano /etc/elysia.env
sudo chown root:elysia /etc/elysia.env
sudo chmod 640 /etc/elysia.env
sudo -u elysia bash -c 'set -a; source /etc/elysia.env; set +a; exec /opt/elysia/.venv/bin/python -u /opt/elysia/elysia.py'
```

填写密钥、音色 ID 和需要的路径。模型和云端客户端初始化成功后可连接 ESP32 验证，按 Ctrl+C 停止。个人声纹录音需在新部署中自行注册，不从公开仓库获取。

## systemd 常驻服务

```bash
sudo cp deployment/ubuntu/elysia.service /etc/systemd/system/elysia.service
sudo systemctl daemon-reload
sudo systemctl enable --now elysia
sudo systemctl status elysia --no-pager
sudo journalctl -u elysia -f
```

服务端已处理 SIGTERM，以便 systemd 停止时释放 TCP 连接和合成连接池。后台服务没有交互终端；要用 `/注册声纹` 等终端命令时，先停止服务，使用上述前台方式注册，随后重新启动服务。

让 ESP32 连接此服务器的可达 IPv4 地址，端口与 `SERVER_PORT` 一致。防火墙和云安全组只允许自己的设备来源访问业务端口；当前协议没有设备认证及 TLS。CPU 内存需求取决于声纹模型和库，不必安装本地大语言模型或本地 TTS 模型。
