"""Keep the agent skill in step with the release."""

import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SKILL = ROOT / ".claude" / "skills" / "rumi"
CODEX_SKILL = ROOT / ".agents" / "skills" / "rumi"
LINK = re.compile(r"\[[^]]*\]\((?!https?://)([^)#]+\.md)(?:#[^)]*)?\)")


def test_the_skill_tracks_the_release_and_its_references_resolve():
    if not SKILL.is_dir():            # a wheel install has no skill beside it
        pytest.skip(f"{SKILL} not present")
    body = (SKILL / "SKILL.md").read_text(encoding="utf-8")
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

    assert f"This skill describes **rumi {version}**" in body
    assert CODEX_SKILL.resolve() == SKILL.resolve()

    failures = []
    for source in SKILL.rglob("*.md"):
        for relative in LINK.findall(source.read_text(encoding="utf-8")):
            if not (source.parent / relative).is_file():
                failures.append(f"{source.relative_to(SKILL)}: {relative}")

    assert not failures, "broken skill references:\n" + "\n".join(failures)
