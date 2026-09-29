# ElysiaSkill

这是供 `elysia.py` 使用的本地爱莉希雅角色技能包。它只保存角色说明、剧情摘要、原创示例和语音表现配置，不包含模型权重、游戏资源或原始语音。

## 文件说明

- `skill.json`：清单、版本和上下文配置。
- `persona.md`：核心人格与互动规则。
- `lore.md`：精简世界观和人物关系。
- `forbidden.md`：防止客服腔、过度悲伤和设定编造。
- `examples.json`：原创少样本对话。
- `voice_config.json`：文本与 VoxCPM 表现建议。
- `loader.py`：统一加载并验证上述文件。

## 自检

在本目录运行：

```powershell
python loader.py
```

正常情况下会显示角色包名称、版本以及示例消息数量。

## 接入原则

1. 将 `system_prompt` 作为系统消息主体。
2. 将 `example_messages` 放在短期记忆之前。
3. 将最近若干轮短期记忆放在示例之后。
4. 最后追加用户当前问题。
5. `voice_config.json` 只用于程序决定表现方式，不应整份发送给语言模型。

推荐顺序：系统角色包 → 示例对话 → 最近对话 → 当前问题。

