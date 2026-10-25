r"""Report settings_t data members that may be unused.

This is a lexical audit rather than a C++ parser. It strips comments and string
literals, extracts ordinary data-member declarations from ``settings_t``, and
counts matching identifiers in the source tree outside the declaration and the
settings constructor/read/write storage plumbing. Treat zero-reference results
as storage-only or unused candidates; common member names can produce false
negatives.

Run from the repository root::

    tools\.venv\Scripts\python.exe tools\check_settings_usage.py
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inl"}
MEMBER_RE = re.compile(
    r"^[ \t]*(?!static\b)(?!using\b)(?!typedef\b)(?!return\b)"
    r"(?:[A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?[\s*&]+)+"
    r"(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Member:
    name: str
    line: int


def strip_comments_and_strings(text: str) -> str:
    """Blank comments and literal contents while preserving lines and offsets."""
    result = list(text)
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            while index < len(text) and text[index] != "\n":
                result[index] = " "
                index += 1
        elif text.startswith("/*", index):
            result[index] = result[index + 1] = " "
            index += 2
            while index < len(text) and not text.startswith("*/", index):
                if text[index] != "\n":
                    result[index] = " "
                index += 1
            if index < len(text):
                result[index] = result[index + 1] = " "
                index += 2
        elif text[index] in {'"', "'"}:
            quote = text[index]
            index += 1
            while index < len(text):
                if text[index] == "\\":
                    result[index] = " "
                    if index + 1 < len(text) and text[index + 1] != "\n":
                        result[index + 1] = " "
                    index += 2
                elif text[index] == quote or text[index] == "\n":
                    index += 1
                    break
                else:
                    result[index] = " "
                    index += 1
        else:
            index += 1
    return "".join(result)


def class_body(text: str, class_name: str) -> tuple[str, int]:
    match = re.search(rf"\bclass\s+{re.escape(class_name)}\s*\{{", text)
    if not match:
        raise ValueError(f"class {class_name!r} was not found")

    body_start = match.end()
    depth = 1
    index = body_start
    while index < len(text) and depth:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise ValueError(f"class {class_name!r} has no closing brace")
    return text[body_start:index - 1], body_start


def find_members(header_text: str) -> list[Member]:
    code = strip_comments_and_strings(header_text)
    body, body_offset = class_body(code, "settings_t")
    members: list[Member] = []
    for match in MEMBER_RE.finditer(body):
        declaration = match.group(0)
        if "(" in declaration or declaration.lstrip().startswith(("struct ", "class ", "enum ")):
            continue
        offset = body_offset + match.start("name")
        members.append(Member(match.group("name"), code.count("\n", 0, offset) + 1))
    return members


def source_files(source_root: Path) -> list[Path]:
    return sorted(
        path for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    )


def strip_settings_storage(code: str) -> str:
    """Blank settings_t constructor/read/write bodies, preserving offsets."""
    patterns = (
        r"\bsettings_t::settings_t\s*\(\s*\)",
        r"\bvoid\s+settings_t::read\s*\(\s*\)",
        r"\bvoid\s+settings_t::write\s*\(\s*\)\s*const",
    )
    result = list(code)
    for pattern in patterns:
        match = re.search(pattern, code)
        if not match:
            continue
        opening = code.find("{", match.end())
        if opening < 0:
            continue
        depth = 1
        index = opening + 1
        while index < len(code) and depth:
            if code[index] == "{":
                depth += 1
            elif code[index] == "}":
                depth -= 1
            index += 1
        for offset in range(match.start(), index):
            if result[offset] != "\n":
                result[offset] = " "
    return "".join(result)


def audit(header: Path, source_root: Path, show_all: bool) -> int:
    header_text = header.read_text(encoding="utf-8", errors="replace")
    members = find_members(header_text)
    files = source_files(source_root)
    code_by_file = {
        path: strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
        for path in files
    }
    implementation = header.with_suffix(".cpp").resolve()
    code_by_file = {
        path: strip_settings_storage(code) if path.resolve() == implementation else code
        for path, code in code_by_file.items()
    }

    unused = 0
    for member in members:
        token = re.compile(rf"\b{re.escape(member.name)}\b")
        references: list[str] = []
        for path, code in code_by_file.items():
            for match in token.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                if path.resolve() == header.resolve() and line == member.line:
                    continue
                references.append(f"{path.as_posix()}:{line}")

        if not references:
            unused += 1
            print(f"STORAGE-ONLY? {member.name} ({header.as_posix()}:{member.line})")
        elif show_all:
            locations = ", ".join(references[:8])
            suffix = f", +{len(references) - 8} more" if len(references) > 8 else ""
            print(f"{len(references):4}  {member.name}: {locations}{suffix}")

    print(
        f"\nChecked {len(members)} settings across {len(files)} files; "
        f"{unused} have no references outside constructor/read/write plumbing."
    )
    return 1 if unused else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=Path("src/app_settings.h"))
    parser.add_argument("--source", type=Path, default=Path("src"))
    parser.add_argument("--all", action="store_true", help="show references for every setting")
    args = parser.parse_args()
    return audit(args.header, args.source, args.all)


if __name__ == "__main__":
    raise SystemExit(main())