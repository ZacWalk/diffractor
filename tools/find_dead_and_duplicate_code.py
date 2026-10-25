"""Find dead-code and duplicate-code candidates in a C/C++ tree.

Two independent heuristic passes, both intentionally self-contained (no
third-party dependency, no compiler database):

  * **Pass 1 - reference counting.** Every function/method *definition* is
    located, then every identifier in the tree is counted. A definition whose
    name is never used outside its own definition/declaration sites is a dead
    code candidate; one used exactly once is an inline/merge candidate.
  * **Pass 2 - duplicate blocks.** Each line is normalised (comments removed,
    all whitespace removed) and used as its own hash. Runs of ``--min-lines``
    or more consecutive normalised lines that appear more than once are
    reported, greedily expanded to their maximal length.

Both passes are *heuristics* - they parse text, not C++ - so treat the output
as a review list, not a verdict. Known false positives:

  * virtual overrides and interface implementations (called through the base),
  * callbacks and functions referenced only from macros or generated code,
  * names reached only via ``&name`` in a table that the stripper mangles,
  * duplicate blocks that are structurally similar but semantically distinct
    (switch dispatch tables, per-format decoders).

Usage (from the repo root)::

    tools\\.venv\\Scripts\\python.exe tools\\find_dead_and_duplicate_code.py src

A Markdown report is written to ``tmp/dead_and_duplicate_code.md`` and a
summary is printed to stdout. The exit code is always 0: this is a review
tool, not a gate.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inl"}

# Words that can precede '(' but never name a function definition.
NON_FUNCTION_NAMES = {
    "if", "else", "for", "while", "do", "switch", "case", "catch", "return",
    "sizeof", "alignof", "decltype", "typeid", "throw", "new", "delete",
    "and", "or", "not", "using", "static_assert", "noexcept", "constexpr",
    "explicit", "template", "typename", "struct", "class", "union", "enum",
    "namespace", "public", "private", "protected", "virtual", "inline",
    "friend", "operator", "co_await", "co_return", "co_yield", "requires",
}

# A definition: optional return type, name, parameter list, trailing
# specifiers, then an opening brace. Parameters may span lines.
DEFINITION_RE = re.compile(
    r"(?<![.\w])(?P<name>~?[A-Za-z_]\w*)\s*"
    r"\([^;{}()]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*(?:\([^()]*\)\s*)?)?"
    r"(?:override\s*|final\s*)*"
    r"(?:->\s*[\w:<>,&*\s]+)?"
    # constructor initialiser list; the leading `member(` shape keeps a
    # ternary such as `f() : std::vector<t>{}` from looking like one
    r"(?::\s*[\w:]+\s*[({][^;{}]*)?"
    r"\{"
)

# A declaration: needs a leading type token, so a bare call `foo();` cannot
# match. Parameters may span lines.
DECLARATION_RE = re.compile(
    r"^[ \t]*(?:(?:virtual|static|inline|explicit|constexpr|friend|extern)\s+)*"
    r"[A-Za-z_][\w:<>,&*\s]*?[\s&*]"
    r"(?<![.\w])(?P<name>~?[A-Za-z_]\w*)\s*"
    r"\([^;{}]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*(?:\([^()]*\)\s*)?)?"
    r"(?:override\s*|final\s*)*"
    r"(?:=\s*(?:0|default|delete)\s*)?;",
    re.MULTILINE,
)

IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")

VIRTUAL_RE = re.compile(r"\b(?:override|final|virtual)\b")

# Reached through a vtable, a function pointer or an OS callback, so a plain
# name search can never find the caller.
ABI_RE = re.compile(
    r"\b(?:STDMETHODCALLTYPE|STDMETHODIMP|STDAPI|__stdcall|__cdecl|__fastcall|"
    r"WINAPI|APIENTRY|CALLBACK|CDECL)\b"
)


def signature_of(code: str, match: re.Match) -> str:
    start = code.rfind("\n", 0, match.start()) + 1
    return code[start:match.end()]


def is_virtual(code: str, match: re.Match) -> bool:
    """True when the definition is an override - reached through a base class."""
    return VIRTUAL_RE.search(signature_of(code, match)) is not None


def is_abi(code: str, match: re.Match) -> bool:
    return ABI_RE.search(signature_of(code, match)) is not None


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literal bodies, preserving offsets."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    if i + 1 < n and text[i + 1] != "\n":
                        out[i + 1] = " "
                    i += 2
                    continue
                if text[i] == quote or text[i] == "\n":
                    break
                out[i] = " "
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


def line_starts(text: str) -> list[int]:
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    return starts


def line_of(starts: list[int], offset: int) -> int:
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= offset:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


class SourceFile:
    def __init__(self, path: Path, root: Path) -> None:
        self.path = path
        self.rel = path.relative_to(root.parent if root.is_dir() else root)
        # newline='' keeps CRLF intact so --verify can restore the exact bytes
        with path.open("r", encoding="utf-8", errors="replace", newline="") as fh:
            self.text = fh.read()
        self.code = strip_comments_and_strings(self.text)
        self.starts = line_starts(self.code)

    def line(self, offset: int) -> int:
        return line_of(self.starts, offset)


def collect_files(roots: list[Path], excludes: list[str]) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        if root.is_file():
            files.append(root)
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.as_posix()
            if any(re.search(pattern, rel) for pattern in excludes):
                continue
            files.append(path)
    return files


# --------------------------------------------------------------------------
# Pass 1 - reference counting
# --------------------------------------------------------------------------

class Candidate:
    def __init__(self, name: str) -> None:
        self.name = name
        self.definitions: list[tuple[SourceFile, int]] = []
        self.declarations = 0
        self.uses: list[tuple[SourceFile, int]] = []
        self.virtual = False
        self.abi = False

    @property
    def bucket(self) -> str:
        if self.abi:
            return "abi"
        if self.virtual:
            return "virtual"
        return "plain"


def analyse_references(files: list[SourceFile], min_name_len: int) -> list[Candidate]:
    candidates: dict[str, Candidate] = {}
    # offsets of definition/declaration name tokens, per file, per name
    anchors: dict[str, dict[str, set[int]]] = defaultdict(lambda: defaultdict(set))
    tokens: dict[str, list[tuple[SourceFile, int]]] = defaultdict(list)

    for f in files:
        key = str(f.path)
        for m in DEFINITION_RE.finditer(f.code):
            name = m.group("name")
            if name in NON_FUNCTION_NAMES or name.startswith("~"):
                continue
            if len(name) < min_name_len:
                continue
            cand = candidates.setdefault(name, Candidate(name))
            cand.definitions.append((f, f.line(m.start("name"))))
            if is_virtual(f.code, m):
                cand.virtual = True
            if is_abi(f.code, m):
                cand.abi = True
            anchors[key][name].add(m.start("name"))
        for m in DECLARATION_RE.finditer(f.code):
            name = m.group("name")
            if name in NON_FUNCTION_NAMES or name.startswith("~"):
                continue
            anchors[key][name].add(m.start("name"))

    for f in files:
        for m in IDENTIFIER_RE.finditer(f.code):
            tokens[m.group(0)].append((f, m.start()))

    for name, cand in candidates.items():
        for f, offset in tokens.get(name, ()):
            if offset in anchors[str(f.path)][name]:
                cand.declarations += 1
                continue
            cand.uses.append((f, f.line(offset)))
        # anchors counted above include the definitions themselves
        cand.declarations -= len(cand.definitions)

    return sorted(candidates.values(), key=lambda c: (len(c.uses), c.name))


# --------------------------------------------------------------------------
# Pass 2 - duplicate blocks
# --------------------------------------------------------------------------

TRIVIAL_LINES = {
    "{", "}", "};", "){", "})", "});", "}else{", "return;", "break;",
    "continue;", "default:", "private:", "public:", "protected:", "else{",
}


def normalise(line: str) -> str:
    return re.sub(r"\s+", "", line)


def significant_lines(files: list[SourceFile], min_chars: int):
    """Yield (file_index, line_number, normalised_text) for meaningful lines."""
    out = []
    for idx, f in enumerate(files):
        for lineno, raw in enumerate(f.code.splitlines(), start=1):
            norm = normalise(raw)
            if not norm or norm in TRIVIAL_LINES or len(norm) < min_chars:
                continue
            if norm.startswith("#include") or norm.startswith("#pragma"):
                continue
            out.append((idx, lineno, norm))
    return out


def analyse_duplicates(files: list[SourceFile], min_lines: int, min_chars: int):
    lines = significant_lines(files, min_chars)
    windows: dict[tuple, list[int]] = defaultdict(list)
    for i in range(len(lines) - min_lines + 1):
        chunk = lines[i:i + min_lines]
        if chunk[0][0] != chunk[-1][0]:
            continue  # window straddles two files
        windows[tuple(c[2] for c in chunk)].append(i)

    consumed: set[int] = set()
    clones = []
    for i in range(len(lines) - min_lines + 1):
        if i in consumed:
            continue
        chunk = lines[i:i + min_lines]
        if chunk[0][0] != chunk[-1][0]:
            continue
        starts = [s for s in windows[tuple(c[2] for c in chunk)] if s not in consumed]
        # keep only non-overlapping occurrences; a run of near-identical lines
        # otherwise reports one clone per starting offset
        disjoint: list[int] = []
        last_end = -1
        for s in starts:
            if s >= last_end:
                disjoint.append(s)
                last_end = s + min_lines
        starts = disjoint
        if len(starts) < 2:
            continue
        length = min_lines
        while True:
            nxt = [s + length for s in starts]
            if any(j >= len(lines) for j in nxt):
                break
            if any(lines[j][0] != lines[s][0] for j, s in zip(nxt, starts)):
                break
            if len({lines[j][2] for j in nxt}) != 1:
                break
            # keep occurrences disjoint
            if any(a + length > b for a, b in zip(starts, starts[1:])):
                break
            length += 1
        occurrences = [(files[lines[s][0]], lines[s][1], lines[s + length - 1][1])
                       for s in starts]
        clones.append((length, occurrences, [lines[i + k][2] for k in range(length)]))
        for s in starts:
            consumed.update(range(s, s + length))

    clones.sort(key=lambda c: (-c[0] * len(c[1]), -c[0]))
    return clones


# --------------------------------------------------------------------------
# Pass 3 - commented-out code
# --------------------------------------------------------------------------

DIVIDER_RE = re.compile(r"^[-=/*_#!<>\s]+$")
STRONG_END_RE = re.compile(r"[;{}]\s*$")
CODE_TOKEN_RE = re.compile(r"->|::|\+\+|--|==|!=|&&|\|\||[;{}=<>()\[\]]")
KEYWORD_RE = re.compile(
    r"\b(if|else|for|while|switch|case|return|break|continue|const|auto|void|int|bool|float|"
    r"double|char|unsigned|static|struct|class|template|typename|namespace|new|delete|nullptr|"
    r"sizeof|std|df|ui|str|assert_true|override|noexcept)\b"
)
PREPROC_RE = re.compile(r"^#\s*(include|define|undef|if|ifdef|ifndef|else|elif|endif|pragma)\b")
DISABLED_BLOCK_RE = re.compile(r"^[ \t]*#[ \t]*if[ \t]+0\b", re.MULTILINE)

# Words that mean the line is a sentence, not source.
PROSE_WORDS = {
    "the", "this", "that", "these", "those", "is", "are", "was", "were", "be", "been", "being",
    "will", "would", "should", "could", "can", "may", "might", "must", "does", "did", "has",
    "have", "had", "its", "we", "our", "you", "your", "they", "them", "their", "there", "here",
    "then", "because", "which", "what", "why", "how", "not", "only", "also", "just", "with",
    "from", "into", "onto", "over", "under", "after", "before", "during", "until", "always",
    "never", "every", "each", "both", "either", "neither", "rather", "instead", "but", "yet",
    "however", "therefore", "thus", "hence", "see", "note", "todo", "fixme", "hack", "purpose",
    "issue", "does", "doesn", "don", "isn", "aren", "wasn", "keep", "keeps", "kept", "still",
    "a", "an", "of", "about", "around", "between", "through", "without", "within", "against",
    "slight", "safety", "invalid", "rest", "same", "other", "another", "such", "way", "whether",
}


def looks_like_code(line: str) -> tuple[bool, bool]:
    """Returns (looks like source, ends in a statement terminator)."""
    s = line.strip().lstrip("*").strip()
    if not s or DIVIDER_RE.match(s):
        return False, False
    if PREPROC_RE.match(s):
        return True, True
    if not CODE_TOKEN_RE.search(s):
        return False, False
    strong = bool(STRONG_END_RE.search(s))
    words = re.findall(r"[A-Za-z_][A-Za-z_]*", s)
    prose = sum(1 for w in words if w.lower() in PROSE_WORDS)
    if prose >= 3 or (prose >= 2 and not strong):
        return False, False
    if strong or KEYWORD_RE.search(s) or re.search(r"[\w\]\)]\s*\([^()]*\)", s):
        return True, strong
    return False, False


def comment_blocks(f: SourceFile) -> list[tuple[int, list[str], bool]]:
    """Runs of consecutive whole-line comments, as (first line, bodies, all `//`)."""
    entries: list[tuple[int, str, bool]] = []
    text = f.text
    i, n = 0, len(text)
    while i < n:
        # f.code has comments blanked, so a difference marks a comment body
        if text[i] == "/" and i + 1 < n and text[i + 1] in "/*" and f.code[i] == " ":
            line_start = text.rfind("\n", 0, i) + 1
            own_line = not text[line_start:i].strip()
            if text[i + 1] == "/":
                end = text.find("\n", i)
                end = n if end < 0 else end
                if own_line:
                    entries.append((f.line(i), text[i + 2:end], True))
                i = end
            else:
                end = text.find("*/", i + 2)
                end = n if end < 0 else end + 2
                if own_line:
                    body = text[i + 2:end - 2]
                    for offset, part in enumerate(body.split("\n")):
                        entries.append((f.line(i) + offset, part, False))
                i = end
        else:
            i += 1

    blocks: list[tuple[int, list[str], bool]] = []
    for lineno, body, slashes in entries:
        if blocks and blocks[-1][0] + len(blocks[-1][1]) == lineno:
            blocks[-1][1].append(body)
            if not slashes:
                blocks[-1] = (blocks[-1][0], blocks[-1][1], False)
        else:
            blocks.append((lineno, [body], slashes))
    return blocks


def line_kind(body: str) -> str:
    s = body.strip().lstrip("*").strip()
    if not s or DIVIDER_RE.match(s):
        return "blank"
    if looks_like_code(body)[0]:
        return "code"
    words = re.findall(r"[A-Za-z_][A-Za-z_]*", s)
    if sum(1 for w in words if w.lower() in PROSE_WORDS) >= 2:
        return "prose"
    if len(words) >= 4 and not CODE_TOKEN_RE.search(s):
        return "prose"
    return "neutral"


def deletable_span(bodies: list[str]) -> tuple[int, int]:
    """The run minus any leading and trailing prose - a stray `else` or `case x:` in the
    middle of commented-out code belongs with it."""
    kinds = [line_kind(b) for b in bodies]
    first = 0
    while first < len(kinds) and kinds[first] in ("prose", "blank"):
        first += 1
    last = len(kinds)
    while last > first and kinds[last - 1] in ("prose", "blank"):
        last -= 1
    return first, last


def analyse_commented_code(files: list[SourceFile]):
    """Comment runs that are mostly source, plus any `#if 0` regions."""
    found = []
    for f in files:
        for start, bodies, slashes in comment_blocks(f):
            flags = [looks_like_code(b) for b in bodies]
            meaningful = [b for b in bodies if b.strip().lstrip("*").strip()]
            code_lines = sum(1 for c, _ in flags if c)
            if not meaningful or not any(s for _, s in flags):
                continue
            if code_lines / len(meaningful) < 0.6:
                continue
            found.append((f, start, len(bodies), code_lines, bodies, "comment" if slashes else "/* */"))

        for m in DISABLED_BLOCK_RE.finditer(f.code):
            depth = 0
            end_line = f.line(m.start())
            for line_no, raw in enumerate(f.text.splitlines()[end_line - 1:], start=end_line):
                stripped = raw.strip()
                if re.match(r"#\s*if", stripped):
                    depth += 1
                elif re.match(r"#\s*endif", stripped):
                    depth -= 1
                    if depth == 0:
                        end_line = line_no
                        break
            span = end_line - f.line(m.start()) + 1
            found.append((f, f.line(m.start()), span, span, [], "#if 0"))

    found.sort(key=lambda x: -x[2])
    return found


def delete_commented(found) -> int:
    """Removes the code segments of `//`-only comment runs, leaving prose in place."""
    per_file: dict[SourceFile, list[tuple[int, int]]] = defaultdict(list)
    removed = 0

    for f, start, span, _code_lines, bodies, kind in found:
        if kind == "comment":
            first, last = deletable_span(bodies)
            if first >= last:
                continue
        elif kind == "/* */":
            # a block comment can only go as a whole, so require it to be all code
            if any(line_kind(b) == "prose" for b in bodies):
                print(f"  skipped (prose inside): {f.rel.as_posix()}:{start}")
                continue
            first, last = 0, span
        else:
            continue

        begin = f.starts[start - 1 + first]
        end_line = start - 1 + last
        end = f.starts[end_line] if end_line < len(f.starts) else len(f.text)
        per_file[f].append((begin, end))
        removed += last - first

    for f, spans in per_file.items():
        text = f.text
        for begin, end in sorted(spans, reverse=True):
            head, tail = text[:begin], text[end:]
            # a comment between two blank lines would otherwise leave a double blank
            if head.rstrip(" \t").endswith("\n") and not tail.split("\n", 1)[0].strip():
                while tail[:1] in (" ", "\t"):
                    tail = tail[1:]
                if tail.startswith("\r\n"):
                    tail = tail[2:]
                elif tail.startswith("\n"):
                    tail = tail[1:]
            text = head + tail
        f.path.write_text(text, encoding="utf-8", newline="")
        print(f"  {f.rel.as_posix()}: {len(spans)} block(s)")

    return removed


# --------------------------------------------------------------------------
# Verification - let the compiler decide
# --------------------------------------------------------------------------

DEFAULT_BUILD = (
    r'"C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild'
    r'\Current\Bin\MSBuild.exe" df.sln /p:Configuration=Debug /p:Platform=x64 '
    r"/m /v:m /nologo"
)

ERROR_NAME_RE = re.compile(r"(?:fatal\s+)?error [A-Z]+\d+: [^\n]*?'(?:[\w:]*::)?(\w+)'")
ERROR_LINE_RE = re.compile(r":\s*(?:fatal\s+)?error [A-Z]+\d+:")


def definition_span(f: SourceFile, line: int) -> tuple[int, int] | None:
    """Byte span of the whole definition whose name token is on `line`."""
    for m in DEFINITION_RE.finditer(f.code):
        if f.line(m.start("name")) != line:
            continue
        depth = 0
        i = m.end() - 1
        while i < len(f.code):
            if f.code[i] == "{":
                depth += 1
            elif f.code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        else:
            return None
        # walk back over the return type, template prefix and attributes,
        # stepping over '::' so a qualified name is not cut in half
        start = 0
        j = m.start("name") - 1
        while j >= 0:
            ch = f.code[j]
            if ch in ";{}":
                start = j + 1
                break
            if ch == ":":
                if f.code[j - 1:j] == ":" or f.code[j + 1:j + 2] == ":":
                    j -= 2
                    continue
                start = j + 1
                break
            if ch == "\n":
                line_start = f.code.rfind("\n", 0, j) + 1
                if f.code[line_start:j].lstrip().startswith("#"):
                    start = j + 1
                    break
            j -= 1
        while start < len(f.code) and f.code[start] in " \t\r\n":
            start += 1
        return start, i + 1
    return None


IN_USE_PATH = Path("tmp/dead_code_in_use.txt")


def widen_for_delete(f: SourceFile, start: int, end: int) -> tuple[int, int]:
    """Grow a definition span to the whole lines it occupies, plus the comment
    block sitting directly above it."""
    start = f.text.rfind("\n", 0, start) + 1
    nl = f.text.find("\n", end)
    end = len(f.text) if nl < 0 else nl + 1
    while start > 0:
        prev_start = f.text.rfind("\n", 0, start - 1) + 1
        stripped = f.code[prev_start:start].strip()
        if not stripped.startswith("//"):
            break
        start = prev_start
    return start, end


def delete_spans(f: SourceFile, spans: list[tuple[int, int]]) -> str:
    eol = "\r\n" if "\r\n" in f.text else "\n"
    text = f.text
    for start, end in sorted(spans, reverse=True):
        head, tail = text[:start], text[end:]
        j = len(head)
        while j > 0 and head[j - 1] in " \t\r\n":
            j -= 1
        i = 0
        while i < len(tail) and tail[i] in " \t\r\n":
            i += 1
        # resume at the start of the kept line so its indentation survives
        nl = tail.rfind("\n", 0, i)
        i = nl + 1 if nl >= 0 else 0
        if j == 0 or i >= len(tail):
            sep = eol
        elif head[j - 1] == "{":
            sep = eol
        else:
            sep = eol * 2
        text = head[:j] + sep + tail[i:]
    return text


def delete_dead(candidates: list[Candidate], files: list[SourceFile]) -> int:
    """Remove verified-dead definitions and any declaration left behind."""
    spans: dict[SourceFile, list[tuple[int, int]]] = defaultdict(list)
    removed = 0
    wanted = {c.name for c in candidates}

    for cand in candidates:
        for f, line in cand.definitions:
            span = definition_span(f, line)
            if span is None:
                print(f"  skipped (no span): {cand.name} in {f.rel.as_posix()}")
                continue
            spans[f].append(widen_for_delete(f, *span))
            removed += 1

    for f in files:
        for m in DECLARATION_RE.finditer(f.code):
            if m.group("name") in wanted:
                spans[f].append(widen_for_delete(f, m.start(), m.end()))

    for f, file_spans in spans.items():
        merged: list[tuple[int, int]] = []
        for start, end in sorted(set(file_spans)):
            if merged and start <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], end))
            else:
                merged.append((start, end))
        f.path.write_text(delete_spans(f, merged), encoding="utf-8", newline="")
        print(f"  {f.rel.as_posix()}: {len(merged)} block(s)")

    return removed


def load_in_use() -> set[str]:
    if not IN_USE_PATH.exists():
        return set()
    return set(IN_USE_PATH.read_text(encoding="utf-8").split())


def verify(candidates: list[Candidate], build_command: str, max_rounds: int) -> None:
    import subprocess

    remaining = {c.name: c for c in candidates}
    backup_dir = Path("tmp/dead_code_backup")
    backup_dir.mkdir(parents=True, exist_ok=True)
    # names the compiler has already rejected in an earlier run
    in_use_path = IN_USE_PATH
    in_use = load_in_use()
    for name in in_use:
        remaining.pop(name, None)
    if in_use:
        print(f"skipping {len(in_use)} names proven in use by an earlier run")
    originals: dict[Path, str] = {}

    def restore() -> None:
        for path, text in originals.items():
            path.write_text(text, encoding="utf-8", newline="")

    def record(names: set[str]) -> None:
        in_use.update(names)
        in_use_path.write_text("\n".join(sorted(in_use)), encoding="utf-8")

    try:
        for round_no in range(1, max_rounds + 1):
            edits: dict[SourceFile, list[tuple[int, int, str]]] = defaultdict(list)
            skipped = []
            for name, cand in remaining.items():
                for f, line in cand.definitions:
                    span = definition_span(f, line)
                    if span is None:
                        skipped.append(name)
                        continue
                    body = f.code[span[0]:span[1]]
                    if len(re.findall(r"(?m)^\s*#\s*if", body)) != \
                            len(re.findall(r"(?m)^\s*#\s*endif", body)):
                        skipped.append(name)
                        continue
                    edits[f].append((span[0], span[1], name))
            restore()
            originals.clear()
            for f, spans in edits.items():
                originals[f.path] = f.text
                (backup_dir / f.path.name).write_text(f.text, encoding="utf-8", newline="")
                text = f.text
                for start, end, name in sorted(spans, reverse=True):
                    text = (text[:start] + f"#if 0 // dead-check {name}\n"
                            + text[start:end] + f"\n#endif // dead-check {name}\n"
                            + text[end:])
                f.path.write_text(text, encoding="utf-8", newline="")

            print(f"round {round_no}: building with {len(remaining)} definitions "
                  f"removed from {len(edits)} files ...", flush=True)
            proc = subprocess.run(build_command, shell=True, capture_output=True,
                                  text=True, errors="replace")
            if proc.returncode == 0:
                restore()
                print(f"\nCONFIRMED DEAD ({len(remaining)}):")
                for name in sorted(remaining):
                    where = ", ".join(f"{f.rel.as_posix()}:{ln}"
                                      for f, ln in remaining[name].definitions)
                    print(f"  {name}  -  {where}")
                if skipped:
                    print(f"\nnot checked (span not resolved): {', '.join(sorted(set(skipped)))}")
                return

            output = proc.stdout + proc.stderr
            blamed = {n for n in ERROR_NAME_RE.findall(output) if n in remaining}
            # any candidate named anywhere in an error line is suspect too
            for line in output.splitlines():
                if not ERROR_LINE_RE.search(line):
                    continue
                blamed.update(n for n in remaining if re.search(rf"\b{n}\b", line))
            if not blamed:
                # the compiler named no candidate: blame every candidate whose
                # definition sits in a file that failed to compile
                failing = set()
                for line in output.splitlines():
                    if not ERROR_LINE_RE.search(line):
                        continue
                    mm = re.match(r"\s*(.+?)\(\d+", line)
                    if mm:
                        failing.add(Path(mm.group(1)).name)
                blamed = {n for n, c in remaining.items()
                          if any(f.path.name in failing for f, _ in c.definitions)}
                if blamed:
                    print(f"  files failed to compile: {', '.join(sorted(failing))}")
            if not blamed:
                restore()
                print("build failed but no candidate could be blamed; "
                      "re-run with --only on a subset to bisect. Build tail:")
                for line in output.splitlines()[-40:]:
                    if line.strip():
                        print("  " + line.strip())
                return
            print(f"  in use (build errors): {', '.join(sorted(blamed))}")
            record(blamed)
            for name in blamed:
                remaining.pop(name, None)
            if not remaining:
                restore()
                print("no candidates left")
                return
        restore()
        print(f"gave up after {max_rounds} rounds; {len(remaining)} unresolved")
    finally:
        restore()


# --------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("roots", nargs="*", default=["src"],
                        help="files or directories to scan (default: src)")
    parser.add_argument("--min-lines", type=int, default=6,
                        help="minimum block length for duplicate detection")
    parser.add_argument("--min-chars", type=int, default=12,
                        help="ignore normalised lines shorter than this")
    parser.add_argument("--min-name-len", type=int, default=4,
                        help="ignore function names shorter than this")
    parser.add_argument("--exclude", action="append", default=[],
                        help="regex matched against the posix path; repeatable")
    parser.add_argument("--top", type=int, default=60,
                        help="duplicate blocks to list in the report")
    parser.add_argument("--report", default="tmp/dead_and_duplicate_code.md",
                        help="markdown report path ('-' to skip)")
    parser.add_argument("--verify", action="store_true",
                        help="prove deadness: remove the candidates, build, and "
                             "drop whatever the compiler still needs")
    parser.add_argument("--delete", action="store_true",
                        help="delete the never-referenced definitions (and any "
                             "declaration left behind) from the sources")
    parser.add_argument("--delete-comments", action="store_true",
                        help="delete commented-out code, leaving prose comments in place")
    parser.add_argument("--only", default="",
                        help="comma-separated names or a path regex to verify")
    parser.add_argument("--build-command", default=DEFAULT_BUILD,
                        help="build command used by --verify")
    parser.add_argument("--rounds", type=int, default=6,
                        help="maximum build rounds for --verify")
    args = parser.parse_args(argv)

    roots = [Path(r) for r in (args.roots or ["src"])]
    missing = [r for r in roots if not r.exists()]
    if missing:
        print(f"error: not found: {', '.join(str(m) for m in missing)}", file=sys.stderr)
        return 2

    paths = collect_files(roots, args.exclude)
    if not paths:
        print("error: no source files found", file=sys.stderr)
        return 2
    files = [SourceFile(p, roots[0]) for p in paths]
    total_lines = sum(f.text.count("\n") + 1 for f in files)

    candidates = analyse_references(files, args.min_name_len)
    in_use = load_in_use()
    unused = [c for c in candidates if not c.uses and c.bucket == "plain"
              and c.name not in in_use]
    rejected = [c for c in candidates if not c.uses and c.bucket == "plain"
                and c.name in in_use]
    virtual_unused = [c for c in candidates if not c.uses and c.bucket == "virtual"]
    abi_unused = [c for c in candidates if not c.uses and c.bucket == "abi"]
    once = [c for c in candidates if len(c.uses) == 1]
    test_only = [c for c in candidates if c.uses
                 and all("test" in f.rel.name for f, _ in c.uses)
                 and not all("test" in f.rel.name for f, _ in c.definitions)]

    clones = analyse_duplicates(files, args.min_lines, args.min_chars)
    duplicate_lines = sum(length * (len(occ) - 1) for length, occ, _ in clones)
    commented = analyse_commented_code(files)
    commented_lines = sum(c[2] for c in commented)

    print(f"scanned {len(files)} files, {total_lines} lines")
    print(f"function definitions found: {len(candidates)}")
    print(f"  never referenced:           {len(unused)}")
    print(f"  ... rejected by compiler:   {len(rejected)}")
    print(f"  ... plus overrides:         {len(virtual_unused)}")
    print(f"  ... plus ABI callbacks:     {len(abi_unused)}")
    print(f"  referenced exactly once:    {len(once)}")
    print(f"  reached only from tests:    {len(test_only)}")
    print(f"duplicate blocks (>= {args.min_lines} lines): {len(clones)}"
          f"  ({duplicate_lines} redundant lines)")
    print(f"commented-out code blocks: {len(commented)}  ({commented_lines} lines)")

    if args.verify:
        subset = unused
        if args.only:
            wanted = {n.strip() for n in args.only.split(",") if n.strip()}
            subset = [c for c in candidates if not c.uses and (
                c.name in wanted
                or any(re.search(args.only, f.rel.as_posix()) for f, _ in c.definitions))]
        if not subset:
            print("nothing to verify")
            return 0
        verify(subset, args.build_command, args.rounds)
        return 0

    if args.delete_comments:
        subset = found = commented
        if args.only:
            subset = [c for c in commented if re.search(args.only, c[0].rel.as_posix())]
        if not subset:
            print("nothing to delete")
            return 0
        print(f"deleting commented-out code from {len(subset)} block(s) ...")
        print(f"removed {delete_commented(subset)} lines")
        print("build and run the tests before committing")
        return 0

    if args.delete:
        subset = unused
        if args.only:
            wanted = {n.strip() for n in args.only.split(",") if n.strip()}
            subset = [c for c in unused if c.name in wanted
                      or any(re.search(args.only, f.rel.as_posix())
                             for f, _ in c.definitions)]
        if not subset:
            print("nothing to delete")
            return 0
        print(f"deleting {len(subset)} definitions ...")
        delete_dead(subset, files)
        print("build and run the tests before committing")
        return 0

    if args.report != "-":
        report = Path(args.report)
        report.parent.mkdir(parents=True, exist_ok=True)
        with report.open("w", encoding="utf-8") as out:
            out.write("# Dead and duplicate code candidates\n\n")
            out.write(f"Scanned {len(files)} files / {total_lines} lines in "
                      f"`{', '.join(str(r) for r in roots)}`.\n\n")
            out.write("Heuristic text analysis - verify every hit before acting. "
                      "Virtual overrides, callbacks and macro-reached functions "
                      "show up as unreferenced.\n\n")

            out.write(f"## Never referenced ({len(unused)})\n\n")
            if rejected:
                out.write(f"{len(rejected)} further candidates were removed by "
                          f"`--verify` because the build then failed - see "
                          f"`{IN_USE_PATH.as_posix()}`.\n\n")
            out.write("| name | defined |\n|---|---|\n")
            for c in unused:
                where = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.definitions)
                out.write(f"| `{c.name}` | {where} |\n")

            out.write(f"\n## Never referenced, declared virtual/override "
                      f"({len(virtual_unused)})\n\n")
            out.write("Usually reached through a base class - low value unless the "
                      "whole interface is dead.\n\n")
            out.write("| name | defined |\n|---|---|\n")
            for c in virtual_unused:
                where = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.definitions)
                out.write(f"| `{c.name}` | {where} |\n")

            out.write(f"\n## Never referenced, ABI/COM callbacks ({len(abi_unused)})\n\n")
            out.write("Reached through a vtable or function pointer - not dead.\n\n")
            out.write("| name | defined |\n|---|---|\n")
            for c in abi_unused:
                where = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.definitions)
                out.write(f"| `{c.name}` | {where} |\n")

            out.write(f"\n## Reached only from tests ({len(test_only)})\n\n")
            out.write("Product code whose only callers are test files.\n\n")
            out.write("| name | defined | used |\n|---|---|---|\n")
            for c in test_only:
                where = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.definitions)
                use = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.uses[:4])
                out.write(f"| `{c.name}` | {where} | {use} |\n")

            out.write(f"\n## Referenced exactly once ({len(once)})\n\n")
            out.write("| name | defined | used |\n|---|---|---|\n")
            for c in once:
                where = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.definitions)
                use = ", ".join(f"{f.rel.as_posix()}:{ln}" for f, ln in c.uses)
                out.write(f"| `{c.name}` | {where} | {use} |\n")

            out.write(f"\n## Duplicate blocks ({len(clones)}, "
                      f"{duplicate_lines} redundant lines)\n\n")
            for length, occurrences, body in clones[:args.top]:
                sites = "; ".join(f"{f.rel.as_posix()}:{a}-{b}" for f, a, b in occurrences)
                out.write(f"### {length} lines x {len(occurrences)} - {sites}\n\n")
                out.write("```\n")
                for line in body[:40]:
                    out.write(line + "\n")
                if len(body) > 40:
                    out.write(f"... ({len(body) - 40} more lines)\n")
                out.write("```\n\n")
            if len(clones) > args.top:
                out.write(f"_{len(clones) - args.top} smaller blocks omitted._\n")

            out.write(f"\n## Commented-out code ({len(commented)}, {commented_lines} lines)\n\n")
            for f, start, span, code_lines, bodies, kind in commented:
                out.write(f"### {f.rel.as_posix()}:{start}-{start + span - 1} "
                          f"({span} lines, {kind})\n\n```\n")
                for body in bodies[:20]:
                    out.write(body.rstrip() + "\n")
                if len(bodies) > 20:
                    out.write(f"... ({len(bodies) - 20} more lines)\n")
                out.write("```\n\n")
        print(f"report: {report.as_posix()}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
