import re
import sys
from pathlib import Path

# 回归检查：JUCE 8 的 juce::String (const char*) 只接受纯 ASCII 输入，
# 含有非 ASCII 字符的字符串字面量必须添加 u8 前缀，
# 或者通过 juce::String::fromUTF8 / juce::CharPointer_UTF8 构造。
# 本脚本扫描 plugin/Source 下所有 C++ 源文件，找出违反该规则的字面量。

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = ROOT / "plugin" / "Source"

LITERAL = re.compile(r'(?P<prefix>u8)?"(?:[^"\\]|\\.)*"')
ALLOWED_WRAPPERS = ("fromUTF8", "CharPointer_UTF8")


def line_offenders(path: Path) -> list[str]:
    offenders = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        for match in LITERAL.finditer(line):
            content = match.group(0)
            if match.group("prefix") == "u8":
                continue
            if not any(ord(c) > 127 for c in content):
                continue
            before = line[: match.start()].rstrip()
            if before.endswith(ALLOWED_WRAPPERS) or before.endswith("fromUTF8 (") or before.endswith("CharPointer_UTF8 ("):
                continue
            offenders.append(f"{path.relative_to(ROOT)}:{line_number}: {line.strip()}")
    return offenders


def main() -> int:
    offenders = []
    for path in sorted(SOURCE_DIR.rglob("*")):
        if path.suffix in (".cpp", ".h", ".mm"):
            offenders.extend(line_offenders(path))

    if offenders:
        print("发现未添加 u8 前缀的非 ASCII 字符串字面量：")
        for line in offenders:
            print(" ", line)
        return 1

    print("检查通过：所有非 ASCII 字符串字面量均已添加 u8 前缀")
    return 0


if __name__ == "__main__":
    sys.exit(main())
