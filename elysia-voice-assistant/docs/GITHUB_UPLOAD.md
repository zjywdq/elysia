# 上传 GitHub

发布内容以仓库文件和交付的 `elysia-voice-assistant-github.zip` 为准，已包含外壳 STL、PCB/原理图工程及预览。声纹模型权重、个人声纹、密钥、编译输出均不包含在交付压缩包中。

## Git 上传（推荐）

先在 GitHub 创建空仓库，例如 `elysia-voice-assistant`，不要自动添加 README 或许可证。然后在本项目根目录运行，将最后的地址替换为自己的仓库地址：

```powershell
git init -b main
git add .
git status --short
git commit -m "Integrate ESP32 firmware and Elysia voice assistant backend"
git remote add origin https://github.com/YOUR_USERNAME/elysia-voice-assistant.git
git push -u origin main
```

GitHub 可使用自己的认证方式。先查看暂存文件列表，确保没有真实 `.env`、`*.local.h`、声纹录音或模型权重。不要在上一级混有其他文档的工作目录执行 `git add .`。

## 网页上传

将 `elysia-voice-assistant-github.zip` 解压，进入其中的 `elysia-voice-assistant` 目录，再把目录内的文件和子目录上传到新仓库根目录。检查 `.gitignore`、`.gitattributes` 是否也被上传。直接上传 ZIP 只会保存压缩文件，不会展示项目代码。

上传前可运行 `python scripts/check_repository.py`。如果已经配置过本地密钥，网页上传时优先使用交付的干净 ZIP 内容，因为网页拖拽不会自动应用 `.gitignore`。

## 上传后

README 展示在仓库首页。克隆者需要自行准备 API 密钥、音色 ID、声纹模型和设备连接参数。有关模型来源及下载的说明已放在 `backend/models/README.md`，有关 Ubuntu 的说明在 `deployment/ubuntu/README.md`。
