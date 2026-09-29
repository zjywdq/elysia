import json
from pathlib import Path


REQUIRED_FILES = (
    "skill.json",
    "persona.md",
    "lore.md",
    "forbidden.md",
    "examples.json",
    "voice_config.json",
)


def load_elysia_skill(skill_dir):
    """加载本地角色包，返回可直接用于 Chat Completions 的结构。"""
    root = Path(skill_dir).resolve()
    missing = [name for name in REQUIRED_FILES if not (root / name).is_file()]
    if missing:
        raise FileNotFoundError(f"角色技能包缺少文件: {', '.join(missing)}")

    manifest = json.loads((root / "skill.json").read_text(encoding="utf-8"))
    examples_data = json.loads(
        (root / "examples.json").read_text(encoding="utf-8")
    )
    voice_config = json.loads(
        (root / "voice_config.json").read_text(encoding="utf-8")
    )

    sections = []
    for filename in ("persona.md", "lore.md", "forbidden.md"):
        sections.append((root / filename).read_text(encoding="utf-8").strip())

    system_prompt = "\n\n---\n\n".join(sections)
    example_messages = []
    for item in examples_data.get("examples", []):
        user_text = str(item.get("user", "")).strip()
        assistant_text = str(item.get("assistant", "")).strip()
        if not user_text or not assistant_text:
            continue
        example_messages.extend([
            {"role": "user", "content": user_text},
            {"role": "assistant", "content": assistant_text},
        ])

    return {
        "manifest": manifest,
        "system_prompt": system_prompt,
        "example_messages": example_messages,
        "voice_config": voice_config,
    }


if __name__ == "__main__":
    loaded = load_elysia_skill(Path(__file__).parent)
    print(f"角色包: {loaded['manifest']['display_name']}")
    print(f"版本: {loaded['manifest']['version']}")
    print(f"示例消息数: {len(loaded['example_messages'])}")
