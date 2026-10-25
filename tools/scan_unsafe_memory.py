"""Find memory-unsafe C++ paradigms that have a safe, equivalent spelling.

This is a *lexical* scanner, not a compiler. Each rule looks for a shape that is
either unbounded by construction (raw C string calls, unchecked pointer
arithmetic) or that keeps a reference alive across an operation that can
invalidate it (iterator invalidation, pointers into containers, dangling
views). Every rule names the safe alternative, because the point of the report
is not "this is a bug" but "this could not have been a bug if it were written
the other way".

Treat the output as a review list. Known false positives:

  * pointer/iterator rules only see one lexical block, so a container mutated
    through an alias, or one whose capacity is reserved elsewhere, is missed,
    and a rebind of the pointer after the mutation is still reported,
  * `.front()` / `.back()` guarded by an `if` several lines up, by a caller
    precondition, or by construction (a container just filled) is reported,
  * Win32 interop legitimately needs C-style buffers and casts,
  * `new` in a factory that immediately wraps the result in a smart pointer.

Suppress a line by putting `unsafe-ok` in a comment on it, ideally with the
reason.

Usage (from the repo root)::

    tools\\.venv\\Scripts\\python.exe tools\\scan_unsafe_memory.py src

A Markdown report is written to ``tmp/unsafe_memory.md`` and a summary is
printed to stdout. The exit code is always 0: this is a review tool, not a
gate.
"""

from __future__ import annotations

import argparse
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inl"}

SUPPRESS = "unsafe-ok"

SEVERITY_ORDER = {"high": 0, "medium": 1, "low": 2}


# --------------------------------------------------------------------------
# Source handling
# --------------------------------------------------------------------------

def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and literal bodies, preserving every offset and newline."""
    out = list(text)
    i, n = 0, len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            blank(i, end)
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and not (i and (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            if open_paren < 0:
                i += 1
                continue
            delim = text[i + 2:open_paren]
            close = text.find(")" + delim + '"', open_paren)
            end = n if close < 0 else close + len(delim) + 2
            blank(i, end)
            i = end
        elif c == "'" and i and (text[i - 1].isalnum() or text[i - 1] == "_") and i + 1 < n and text[i + 1].isdigit():
            i += 1  # digit separator, as in 1'000'000
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] == "\n":
                    break
                j += 1
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


def line_starts(text: str) -> list[int]:
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    return starts


class SourceFile:
    def __init__(self, path: Path, root: Path) -> None:
        self.path = path
        base = root.parent if root.is_dir() else root.parent
        try:
            self.rel = path.relative_to(base).as_posix()
        except ValueError:
            self.rel = path.as_posix()
        self.text = path.read_text(encoding="utf-8", errors="replace")
        self.code = strip_comments_and_strings(self.text)
        self.starts = line_starts(self.code)
        self.lines = self.text.splitlines()
        self.code_lines = self.code.splitlines()

    def line(self, offset: int) -> int:
        lo, hi = 0, len(self.starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def source_line(self, lineno: int) -> str:
        return self.lines[lineno - 1].strip() if 0 < lineno <= len(self.lines) else ""

    def window(self, lineno: int, before: int, after: int) -> str:
        lo = max(0, lineno - 1 - before)
        hi = min(len(self.code_lines), lineno + after)
        return "\n".join(self.code_lines[lo:hi])

    def block_end(self, offset: int) -> int:
        """End of the innermost `{}` block containing `offset` (or EOF)."""
        depth = 0
        i = offset
        n = len(self.code)
        while i < n:
            c = self.code[i]
            if c == "{":
                depth += 1
            elif c == "}":
                if depth == 0:
                    return i
                depth -= 1
            i += 1
        return n


def matching_brace(code: str, open_index: int) -> int:
    depth = 0
    for i in range(open_index, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(code)


def call_args(code: str, open_paren: int) -> tuple[list[str], int]:
    """Split a call's top-level arguments. `open_paren` indexes the '('."""
    depth = 0
    args: list[str] = []
    start = open_paren + 1
    for i in range(open_paren, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(code[start:i])
                return args, i
        elif c == "," and depth == 1:
            args.append(code[start:i])
            start = i + 1
    return args, len(code)


# --------------------------------------------------------------------------
# Findings
# --------------------------------------------------------------------------

@dataclass
class Finding:
    rule: str
    severity: str
    rel: str
    line: int
    text: str
    detail: str


RULES: dict[str, tuple[str, str]] = {}  # id -> (title, safe alternative)


def rule(rule_id: str, title: str, fix: str):
    RULES[rule_id] = (title, fix)

    def decorate(fn):
        fn.rule_id = rule_id
        return fn

    return decorate


CHECKS: list = []


def check(fn):
    CHECKS.append(fn)
    return fn


# --------------------------------------------------------------------------
# Rule 1 - unbounded C string and conversion calls
# --------------------------------------------------------------------------

UNBOUNDED_CALLS = {
    "strcpy": "high", "strcat": "high", "sprintf": "high", "vsprintf": "high",
    "gets": "high", "wcscpy": "high", "wcscat": "high", "swprintf": "high",
    "wsprintf": "high", "wsprintfW": "high", "vswprintf": "high",
    "strtok": "high", "wcstok": "high", "mbstowcs": "high", "wcstombs": "high",
    "strncpy": "medium", "strncat": "medium", "wcsncpy": "medium", "wcsncat": "medium",
    "sscanf": "medium", "swscanf": "medium", "_snprintf": "medium", "_snwprintf": "medium",
    "atoi": "medium", "atol": "medium", "atof": "medium", "_atoi64": "medium",
    "itoa": "medium", "_itoa": "medium", "ltoa": "medium", "_ltoa": "medium",
}

UNBOUNDED_RE = re.compile(r"(?<![\w:.>])(?:std::)?(" + "|".join(sorted(UNBOUNDED_CALLS, key=len, reverse=True)) + r")\s*\(")

rule("c-string-call", "Unbounded or lossy C string call",
     "`std::format` / `str::` helpers for building, `std::from_chars` for parsing; both carry the "
     "destination size or report failure instead of writing past the end.")


@check
def check_c_string_calls(f: SourceFile) -> list[Finding]:
    out = []
    for m in UNBOUNDED_RE.finditer(f.code):
        name = m.group(1)
        out.append(Finding("c-string-call", UNBOUNDED_CALLS[name], f.rel, f.line(m.start()),
                           f.source_line(f.line(m.start())), f"`{name}` writes or parses without a checked bound"))
    return out


rule("stack-alloca", "Stack allocation with a runtime size",
     "A fixed `std::array`, or a heap `df::blob` / `std::vector` when the size is data-derived; "
     "`alloca` has no failure mode short of a stack overflow.")

ALLOCA_RE = re.compile(r"(?<![\w:.>])(?:_?alloca|_malloca)\s*\(")


@check
def check_alloca(f: SourceFile) -> list[Finding]:
    return [Finding("stack-alloca", "high", f.rel, f.line(m.start()), f.source_line(f.line(m.start())),
                    "runtime-sized stack allocation")
            for m in ALLOCA_RE.finditer(f.code)]


# --------------------------------------------------------------------------
# Rule 2 - raw byte moves with a computed length
# --------------------------------------------------------------------------

MEM_RE = re.compile(r"(?<![\w:.>])(?:std::)?(memcpy|memmove|memset)\s*\(")

rule("raw-byte-move", "Byte move whose length is computed, not `sizeof`",
     "`std::copy_n` / `std::ranges::copy` / `std::fill` over a `df::span`, or a helper that takes "
     "both spans, so the length cannot outlive the buffer it was computed from.")


@check
def check_raw_byte_move(f: SourceFile) -> list[Finding]:
    out = []
    for m in MEM_RE.finditer(f.code):
        args, _ = call_args(f.code, m.end() - 1)
        if len(args) < 3:
            continue
        length = args[2].strip()
        if re.match(r"^sizeof\b", length) or re.fullmatch(r"\d+", length):
            continue
        lineno = f.line(m.start())
        out.append(Finding("raw-byte-move", "medium", f.rel, lineno, f.source_line(lineno),
                           f"`{m.group(1)}` length `{length[:60]}` is not tied to the destination type"))
    return out


# --------------------------------------------------------------------------
# Rule 3 - unsigned subtraction that underflows when empty
# --------------------------------------------------------------------------

SIZE_SUB_RE = re.compile(r"[\w\)\]]\.(size|length)\s*\(\s*\)\s*-\s*(?!-)")
# Anything that establishes the size is large enough, or that clamps the result.
GUARD_RE = re.compile(
    r"\bempty\s*\(\s*\)|\b(?:std::)?(?:min|max|clamp)\b|\bassert"
    r"|\.(?:size|length)\s*\(\s*\)\s*[<>]"          # size() > k
    r"|[<>]=?\s*[\w.\->:]*\.(?:size|length)\s*\(\s*\)"  # k < x.size()
    r"|>\s*0|>=\s*1")

rule("size-underflow", "Subtraction from an unsigned size",
     "Compare before subtracting (`if (n > k)`), or use a signed intermediate. `size() - k` on an "
     "empty or short container wraps to a huge value and turns the next index or length into an "
     "out-of-bounds access.")


@check
def check_size_underflow(f: SourceFile) -> list[Finding]:
    out = []
    for m in SIZE_SUB_RE.finditer(f.code):
        lineno = f.line(m.start())
        if GUARD_RE.search(f.window(lineno, 6, 1)):
            continue
        out.append(Finding("size-underflow", "medium", f.rel, lineno, f.source_line(lineno),
                           "no visible emptiness or magnitude guard"))
    return out


# --------------------------------------------------------------------------
# Rule 4 - index loops (iterators vs indexers)
# --------------------------------------------------------------------------

INDEX_LOOP_RE = re.compile(
    r"for\s*\(\s*(?:const\s+)?(?P<type>(?:std::)?(?:size_t|int|unsigned(?:\s+\w+)?|long|short|"
    r"u?int(?:8|16|32|64)_t|ptrdiff_t|auto|\w+::size_type))\s+(?P<var>\w+)\s*=\s*0\s*;\s*"
    r"(?P=var)\s*(?P<cmp><=|<)\s*(?P<bound>[^;]+?)\s*;\s*(?:\+\+(?P=var)|(?P=var)\+\+|(?P=var)\s*\+=\s*1)\s*\)")

SIGNED_TYPES = re.compile(r"^(?:int|long|short|int(?:8|16|32|64)_t|ptrdiff_t)$")

rule("index-loop", "Index loop over a container that has iterators",
     "A range-`for` (or `std::ranges` algorithm) cannot run off the end, cannot mismatch the "
     "counter against the bound, and does not need the subscript to be revalidated after the body "
     "mutates anything.")

rule("signed-index", "Signed counter compared against an unsigned size",
     "Use `size_t` (or `std::ssize` on the bound) so the comparison does not convert; better, use a "
     "range-`for`.")

rule("inclusive-bound", "Loop condition uses `<=` against a size or count",
     "Half-open bounds (`< size()`). An inclusive bound against a count is an off-by-one whenever "
     "the bound is a length rather than a last index.")


@check
def check_index_loops(f: SourceFile) -> list[Finding]:
    out = []
    for m in INDEX_LOOP_RE.finditer(f.code):
        lineno = f.line(m.start())
        bound = m.group("bound").strip()
        var = m.group("var")
        ctype = m.group("type").replace("std::", "").strip()
        body_start = f.code.find("{", m.end())
        body = ""
        if 0 <= body_start < m.end() + 4:
            body = f.code[body_start:matching_brace(f.code, body_start)]
        indexes = re.search(re.escape(var) + r"\s*\]", body) is not None
        sized = re.search(r"\.(size|length)\s*\(\s*\)\s*$|\bcount\b|\blen\b|\bsize\b", bound) is not None

        if m.group("cmp") == "<=" and sized:
            out.append(Finding("inclusive-bound", "high", f.rel, lineno, f.source_line(lineno),
                               f"`{var} <= {bound[:50]}`"))
        if not indexes:
            continue
        if re.search(r"\.(size|length)\s*\(\s*\)", bound):
            out.append(Finding("index-loop", "low", f.rel, lineno, f.source_line(lineno),
                               f"subscripts with `{var}` over `{bound[:50]}`"))
            if SIGNED_TYPES.match(ctype):
                out.append(Finding("signed-index", "low", f.rel, lineno, f.source_line(lineno),
                                   f"`{ctype} {var}` compared against `{bound[:50]}`"))
    return out


# --------------------------------------------------------------------------
# Rule 5 - iterator invalidation inside a range-for
# --------------------------------------------------------------------------

RANGE_FOR_RE = re.compile(r"for\s*\(\s*(?:const\s+)?(?:auto|[\w:]+)\s*[&*]{0,2}\s*\w+\s*:\s*(?P<range>[^);]+)\)")
MUTATORS = ("push_back", "emplace_back", "emplace", "insert", "erase", "clear",
            "resize", "reserve", "pop_back", "pop_front", "assign", "push_front")


def mutator_re(owner: str) -> re.Pattern:
    """`owner` mutated by name. The lookbehind stops `x` from matching `_x` or `a.x`."""
    return re.compile(r"(?<![\w.>])" + re.escape(owner) + r"\s*\.\s*(" + "|".join(MUTATORS) + r")\s*\(")


def escapes_after(code: str, pos: int) -> bool:
    """True when the mutation is an early exit - `clear(); return;` never reaches the reuse."""
    tail = code[pos:pos + 400]
    stop = tail.find("}")
    return re.search(r"\b(return|throw|break|continue)\b", tail[:stop if stop >= 0 else len(tail)]) is not None

rule("range-for-mutation", "Container mutated while a range-`for` iterates it",
     "Collect into a second container and swap, use `std::erase_if`, or iterate by index with a "
     "re-read bound. A reallocation inside the body invalidates the loop's iterators.")


@check
def check_range_for_mutation(f: SourceFile) -> list[Finding]:
    out = []
    for m in RANGE_FOR_RE.finditer(f.code):
        expr = m.group("range").strip()
        name = re.fullmatch(r"[\w.\->:]+", expr)
        if not name:
            continue
        if expr.endswith(")") or "(" in expr:
            continue
        body_start = f.code.find("{", m.end())
        if body_start < 0 or body_start > m.end() + 4:
            continue
        body = f.code[body_start:matching_brace(f.code, body_start)]
        hit = mutator_re(expr).search(body)
        if not hit:
            continue
        if escapes_after(body, hit.end()):
            continue
        lineno = f.line(body_start + hit.start())
        out.append(Finding("range-for-mutation", "high", f.rel, lineno, f.source_line(lineno),
                           f"`{expr}.{hit.group(1)}()` inside `for (... : {expr})` at line {f.line(m.start())}"))
    return out


# --------------------------------------------------------------------------
# Rule 6 - pointer or reference into a container that is then resized
# --------------------------------------------------------------------------

ALIAS_RE = re.compile(
    r"(?:auto|[\w:]+)\s*(?P<kind>[*&])\s*(?P<var>\w+)\s*=\s*(?P<owner>[\w.\->:]+?)\s*"
    r"(?:\.\s*(?:data|front|back|begin|end)\s*\(\s*\)|\[)")

rule("alias-then-resize", "Pointer or reference into a container outlives a reallocation",
     "Re-fetch after the mutation, hold an index, or reserve capacity up front. `data()`, `&v[i]`, "
     "`front()` and `back()` are all invalidated by `push_back` / `insert` / `resize`.")


@check
def check_alias_then_resize(f: SourceFile) -> list[Finding]:
    out = []
    for m in ALIAS_RE.finditer(f.code):
        owner = m.group("owner")
        if owner.endswith(")") or not re.fullmatch(r"[\w.\->:]+", owner):
            continue
        var = m.group("var")
        end = f.block_end(m.end())
        tail = f.code[m.end():end]
        hit = mutator_re(owner).search(tail)
        if not hit:
            continue
        if escapes_after(tail, hit.end()):
            continue
        # a later rebind of the alias makes the earlier one moot
        rebind = re.search(r"\b" + re.escape(var) + r"\s*=", tail[hit.end():])
        if rebind:
            continue
        if not re.search(r"\b" + re.escape(var) + r"\b", tail[hit.end():]):
            continue
        lineno = f.line(m.start())
        out.append(Finding("alias-then-resize", "high", f.rel, lineno, f.source_line(lineno),
                           f"`{var}` aliases `{owner}`, which is `{hit.group(1)}()`-ed at line "
                           f"{f.line(m.end() + hit.start())} and `{var}` is used after"))
    return out


# --------------------------------------------------------------------------
# Rule 7 - views onto temporaries
# --------------------------------------------------------------------------

DANGLING_RE = re.compile(
    r"(?:const\s+)?(?:std::(?:u8)?string_view|std::string_view|const\s+char\s*\*|const\s+wchar_t\s*\*)"
    r"\s+(?P<var>\w+)\s*=\s*(?P<init>[^;]+);")

rule("dangling-view", "View or raw pointer bound to a temporary",
     "Bind the owner to a named local first, or store the owning `std::string`. A `string_view` or "
     "`c_str()` taken from a temporary dangles at the end of the full expression.")


@check
def check_dangling_view(f: SourceFile) -> list[Finding]:
    out = []
    for m in DANGLING_RE.finditer(f.code):
        init = m.group("init").strip()
        if not re.search(r"\)\s*\.\s*(c_str|data)\s*\(\s*\)\s*$", init):
            continue
        lineno = f.line(m.start())
        out.append(Finding("dangling-view", "high", f.rel, lineno, f.source_line(lineno),
                           f"`{m.group('var')}` points into the result of `{init[:60]}`"))
    return out


RETURN_CSTR_RE = re.compile(r"\breturn\s+([\w.\->:\[\]]+)\s*\.\s*(?:c_str|data)\s*\(\s*\)\s*;")

rule("return-inner-pointer", "Returning `c_str()` / `data()`",
     "Return the owning `std::string` (or a view whose owner the caller keeps). The pointer is only "
     "valid while the returned-from object lives and is unmodified.")


ACCESSOR_RE = re.compile(r"\b(?:data|c_str|begin|end|cbegin|cend|pixels|pixels_line)\s*\(\s*\)\s*(?:const\s*)?(?:noexcept\s*)?\{")


@check
def check_return_cstr(f: SourceFile) -> list[Finding]:
    out = []
    for m in RETURN_CSTR_RE.finditer(f.code):
        lineno = f.line(m.start())
        line = f.code_lines[lineno - 1] if lineno <= len(f.code_lines) else ""
        if ACCESSOR_RE.search(line):  # an accessor forwarding to its own storage
            continue
        out.append(Finding("return-inner-pointer", "medium", f.rel, lineno, f.source_line(lineno),
                           f"returns a pointer into `{m.group(1)}`"))
    return out


# --------------------------------------------------------------------------
# Rule 8 - unchecked element access
# --------------------------------------------------------------------------

# `top()` is a rectangle edge in this codebase, so only the sequence accessors are listed.
FRONT_BACK_RE = re.compile(r"[\w\)\]]\s*(?:\.|->)\s*(front|back)\s*\(\s*\)")
EMPTY_GUARD_RE = re.compile(
    r"\bempty\s*\(\s*\)|\bsize\s*\(\s*\)|!=\s*end\s*\(|\bassert|\bis_empty\b|\bany\w*\s*\("
    r"|\bcount\b|\bhas_\w+\s*\(|!=\s*npos|\bbegin\s*\(\s*\)")

rule("unchecked-element", "`front()` / `back()` with no visible emptiness guard",
     "Test `empty()` first, or use an accessor that returns an optional / a documented empty "
     "sentinel. On an empty container these are undefined behaviour, not a throw.")


@check
def check_front_back(f: SourceFile) -> list[Finding]:
    out = []
    for m in FRONT_BACK_RE.finditer(f.code):
        lineno = f.line(m.start())
        line = f.code_lines[lineno - 1] if lineno <= len(f.code_lines) else ""
        if re.search(r"\b(?:front|back)\s*\(\s*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", line):
            continue  # an accessor forwarding to its own storage
        if EMPTY_GUARD_RE.search(f.window(lineno, 8, 2)):
            continue
        out.append(Finding("unchecked-element", "medium", f.rel, lineno, f.source_line(lineno),
                           f"`{m.group(1)}()` unguarded"))
    return out


FIND_RE = re.compile(r"(?:auto|[\w:]+)\s+(?P<var>\w+)\s*=\s*(?P<owner>[\w.\->:]+)\s*\.\s*(?:find|find_if|lower_bound|upper_bound)\s*\(")

rule("unchecked-find", "Result of `find()` dereferenced without an end check",
     "Compare against `end()` (or use `contains()` / `try_get`) before dereferencing. `end()` is a "
     "valid iterator value and dereferencing it reads past the container.")


@check
def check_unchecked_find(f: SourceFile) -> list[Finding]:
    out = []
    for m in FIND_RE.finditer(f.code):
        var = m.group("var")
        end = f.block_end(m.end())
        tail = f.code[m.end():min(end, m.end() + 1200)]
        deref = re.search(r"(?:\*\s*" + re.escape(var) + r"\b|\b" + re.escape(var) + r"\s*->)", tail)
        if not deref:
            continue
        # `!= end()`, or - for the lookups here that answer a pointer - a truthiness test
        name = re.escape(var)
        guard = re.search(r"\b" + name + r"\s*(?:!=|==)\s*[\w.\->:]*\s*(?:end|cend)\s*\("
                          r"|[(!&|?]\s*" + name + r"\s*[)&|?]"
                          r"|\b" + name + r"\s*(?:&&|\|\||\?)", tail[:deref.start()])
        if guard:
            continue
        lineno = f.line(m.start())
        out.append(Finding("unchecked-find", "high", f.rel, lineno, f.source_line(lineno),
                           f"`{var}` dereferenced at line {f.line(m.end() + deref.start())} with no `!= end()` between"))
    return out


MOVE_RE = re.compile(r"std::move\s*\(\s*(?P<var>[a-zA-Z_]\w*)\s*\)")

rule("use-after-move", "Named object used after it was moved from",
     "Move last, or move into a fresh name. A moved-from object is only guaranteed to be "
     "destructible and assignable; reading it back reads whatever the move left behind.")


def in_capture_list(code: str, pos: int) -> bool:
    """True when `pos` sits inside a lambda's `[...]`, where `x = std::move(x)` rebinds a new `x`."""
    depth = 0
    for i in range(pos - 1, max(0, pos - 600), -1):
        c = code[i]
        if c in ")]}":
            depth += 1
        elif c == "[":
            if depth == 0:
                return True
            depth -= 1
        elif c in "({":
            if depth == 0:
                return False
            depth -= 1
        elif c == ";" and depth == 0:
            return False
    return False


def in_member_init(code: str, pos: int) -> bool:
    """True when `pos` sits in a constructor's `: a(x), b(y)` list, which has no enclosing block."""
    depth = 0
    for i in range(pos - 1, max(0, pos - 2000), -1):
        c = code[i]
        if c in ")]":
            depth += 1
        elif c == "(":
            if depth:
                depth -= 1
        elif c == "[":
            if not depth:
                return False
            depth -= 1
        elif depth == 0:
            if c == ":":
                return code[i - 1:i] != ":" and code[i + 1:i + 2] != ":"
            if c in ";{}":
                return False
    return False


@check
def check_use_after_move(f: SourceFile) -> list[Finding]:
    out = []
    for m in MOVE_RE.finditer(f.code):
        var = m.group("var")
        if var == "this":
            continue
        before = f.code[max(0, m.start() - 60):m.start()]
        # `x = std::move(x)` is an init-capture: the name is rebound, not reused
        if re.search(r"(?<![\w.>])" + re.escape(var) + r"\s*=\s*$", before):
            continue
        if in_capture_list(f.code, m.start()) or in_member_init(f.code, m.start()):
            continue
        end = f.block_end(m.end())
        tail = f.code[m.end():end]
        # a reassignment, reset or clear puts the object back into a known state
        revive = re.search(r"(?<![\w.>])" + re.escape(var) + r"\s*(?:=(?!=)|\.\s*(?:clear|reset|assign)\s*\()", tail)
        limit = revive.start() if revive else len(tail)
        use = re.search(r"(?<![\w.>])" + re.escape(var) + r"\s*(?:\.|->|\[)", tail[:limit])
        if not use:
            continue
        # within the same full expression only one of a ternary's arms runs; that is an ordering
        # question, not a stale read
        statement_end = tail.find(";")
        if statement_end < 0 or use.start() < statement_end:
            continue
        lineno = f.line(m.start())
        out.append(Finding("use-after-move", "high", f.rel, lineno, f.source_line(lineno),
                           f"`{var}` moved here and read again at line {f.line(m.end() + use.start())}"))
    return out


# --------------------------------------------------------------------------
# Rule 9 - casts
# --------------------------------------------------------------------------

C_CAST_RE = re.compile(r"(?<![\w>])\(\s*(?:const\s+|unsigned\s+|signed\s+|struct\s+)*[A-Za-z_][\w:]*\s*(\*+)\s*\)\s*(?=[&\w(])")

rule("c-style-pointer-cast", "C-style pointer cast",
     "`static_cast` / `std::bit_cast` / `reinterpret_cast`. A C-style cast silently becomes whichever "
     "of those compiles, including a `const_cast` nobody intended.")


@check
def check_c_casts(f: SourceFile) -> list[Finding]:
    out = []
    for m in C_CAST_RE.finditer(f.code):
        lineno = f.line(m.start())
        line = f.code_lines[lineno - 1] if lineno <= len(f.code_lines) else ""
        if re.search(r"\b(operator|typedef|using|virtual)\b", line):
            continue
        out.append(Finding("c-style-pointer-cast", "low", f.rel, lineno, f.source_line(lineno),
                           "unqualified pointer cast"))
    return out


BYTE_TYPES = {"void", "char", "uint8_t", "int8_t", "unsigned char", "signed char", "BYTE", "std::byte"}
REINTERPRET_RE = re.compile(r"(?:reinterpret_cast|std::bit_cast|bit_cast)\s*<\s*(?P<type>(?:const\s+)?[\w:\s]+?)\s*(?P<stars>\*+)\s*>\s*\(")

rule("pointer-type-pun", "Reinterpreting an offset byte address as a typed object",
     "`std::memcpy` (or the `read_stream::peek<T>` helper) into a value. A pointer produced by byte "
     "arithmetic carries no alignment guarantee, so casting it to `T*` and dereferencing is "
     "undefined even where the hardware tolerates it.")


@check
def check_reinterpret(f: SourceFile) -> list[Finding]:
    out = []
    for m in REINTERPRET_RE.finditer(f.code):
        target = re.sub(r"\bconst\b", "", m.group("type")).strip()
        if target in BYTE_TYPES or len(m.group("stars")) > 1:
            continue
        args, _ = call_args(f.code, m.end() - 1)
        operand = args[0] if args else ""
        # A base address from an allocator or an API is aligned; one computed with arithmetic is not.
        if not re.search(r"[+\-]", operand.replace("->", ".")):
            continue
        lineno = f.line(m.start())
        out.append(Finding("pointer-type-pun", "low", f.rel, lineno, f.source_line(lineno),
                           f"reinterprets `{operand.strip()[:50]}` as `{target}*`"))
    return out


# --------------------------------------------------------------------------
# Rule 10 - ownership
# --------------------------------------------------------------------------

NEW_RE = re.compile(r"(?<![\w:.>])new\s+(?!\()(?P<type>[\w:]+)")
DELETE_RE = re.compile(r"(?<![\w:.>])delete\s*(?:\[\s*\])?\s+[\w:*(]")

rule("raw-ownership", "Raw `new` / `delete`",
     "`std::make_unique` / `std::make_shared`, or a container. A raw owning pointer leaks on every "
     "early return and every throw between the `new` and the `delete`.")


@check
def check_raw_ownership(f: SourceFile) -> list[Finding]:
    out = []
    for m in NEW_RE.finditer(f.code):
        before = f.code[max(0, m.start() - 40):m.start()]
        if re.search(r"(?:::)?new\s*$|placement", before):
            continue
        lineno = f.line(m.start())
        line = f.code_lines[lineno - 1] if lineno <= len(f.code_lines) else ""
        if "make_unique" in line or "make_shared" in line or "reset(" in line:
            continue
        out.append(Finding("raw-ownership", "low", f.rel, lineno, f.source_line(lineno),
                           f"`new {m.group('type')}`"))
    for m in DELETE_RE.finditer(f.code):
        lineno = f.line(m.start())
        out.append(Finding("raw-ownership", "low", f.rel, lineno, f.source_line(lineno), "explicit `delete`"))
    return out


# --------------------------------------------------------------------------
# Rule 11 - background work capturing by reference
# --------------------------------------------------------------------------

QUEUE_RE = re.compile(r"\b(queue_async|queue_ui|queue_database|queue_crc|queue_work|std::async|create_thread)\s*\(")
DEFAULT_CAPTURE_RE = re.compile(r"\[\s*(?:&|=)\s*\]")

rule("default-capture-async", "Default capture on a lambda handed to another thread",
     "Capture each value explicitly, by value or by move. `[&]` / `[=]` capture whatever the body "
     "happens to name, including `this`, and the caller's frame is gone by the time the queue runs it.")


@check
def check_default_capture(f: SourceFile) -> list[Finding]:
    out = []
    for m in QUEUE_RE.finditer(f.code):
        args, close = call_args(f.code, m.end() - 1)
        text = f.code[m.end():close]
        hit = DEFAULT_CAPTURE_RE.search(text)
        if not hit:
            continue
        lineno = f.line(m.end() + hit.start())
        out.append(Finding("default-capture-async", "high", f.rel, lineno, f.source_line(lineno),
                           f"default capture passed to `{m.group(1)}`"))
    return out


# --------------------------------------------------------------------------
# Rule 12 - fixed stack buffers
# --------------------------------------------------------------------------

STACK_BUFFER_RE = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?(?P<type>char|wchar_t|uint8_t|BYTE|TCHAR|WCHAR|CHAR)\s+"
    r"(?P<var>\w+)\s*\[\s*(?P<size>[\w:+ ]+)\s*\]\s*(?:=|;)", re.MULTILINE)

rule("stack-buffer", "Fixed-size character buffer",
     "`std::array` (which keeps its size with it) or `std::string` / `df::blob`. A bare `T buf[N]` "
     "decays to a pointer at the first call and the bound stops travelling with it.")


@check
def check_stack_buffers(f: SourceFile) -> list[Finding]:
    out = []
    for m in STACK_BUFFER_RE.finditer(f.code):
        lineno = f.line(m.start())
        out.append(Finding("stack-buffer", "low", f.rel, lineno, f.source_line(lineno),
                           f"`{m.group('type')} {m.group('var')}[{m.group('size').strip()}]`"))
    return out


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

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


def scan(files: list[Path], root: Path, rules: set[str] | None) -> list[Finding]:
    findings: list[Finding] = []
    seen: set[tuple[str, str, int, str]] = set()
    for path in files:
        f = SourceFile(path, root)
        for fn in CHECKS:
            for finding in fn(f):
                if rules and finding.rule not in rules:
                    continue
                if SUPPRESS in f.source_line(finding.line):
                    continue
                key = (finding.rule, finding.rel, finding.line, finding.detail)
                if key in seen:
                    continue
                seen.add(key)
                findings.append(finding)
    return findings


def write_report(findings: list[Finding], out_path: Path, roots: list[Path], scanned: int,
                 max_per_rule: int) -> None:
    by_rule: dict[str, list[Finding]] = defaultdict(list)
    for x in findings:
        by_rule[x.rule].append(x)

    lines: list[str] = []
    lines.append("# Unsafe memory paradigms\n")
    lines.append(f"Scanned {scanned} files under {', '.join(str(r) for r in roots)}. "
                 f"{len(findings)} findings across {len(by_rule)} rules.\n")
    lines.append("Every rule names a spelling that removes the failure mode. This is a lexical "
                 "scanner, so confirm each finding before changing code.\n")

    lines.append("| Rule | Severity | Count | Files | What it finds |")
    lines.append("| --- | --- | ---: | ---: | --- |")
    for rule_id in sorted(by_rule, key=lambda r: (min(SEVERITY_ORDER[x.severity] for x in by_rule[r]),
                                                  -len(by_rule[r]))):
        items = by_rule[rule_id]
        worst = min(items, key=lambda x: SEVERITY_ORDER[x.severity]).severity
        files = len({x.rel for x in items})
        lines.append(f"| `{rule_id}` | {worst} | {len(items)} | {files} | {RULES[rule_id][0]} |")
    lines.append("")

    for rule_id in sorted(by_rule, key=lambda r: (min(SEVERITY_ORDER[x.severity] for x in by_rule[r]),
                                                  -len(by_rule[r]))):
        items = sorted(by_rule[rule_id], key=lambda x: (SEVERITY_ORDER[x.severity], x.rel, x.line))
        title, fix = RULES[rule_id]
        lines.append(f"## `{rule_id}` - {title}\n")
        lines.append(f"**Safe alternative.** {fix}\n")
        counts = Counter(x.rel for x in items)
        lines.append(f"{len(items)} findings in {len(counts)} files. "
                     f"Busiest: {', '.join(f'{p} ({n})' for p, n in counts.most_common(5))}.\n")
        shown = items[:max_per_rule]
        for x in shown:
            lines.append(f"- {x.rel}:{x.line} ({x.severity}) - {x.detail}")
            lines.append(f"  - `{x.text[:160]}`")
        if len(items) > len(shown):
            lines.append(f"- ... {len(items) - len(shown)} more")
        lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


# --------------------------------------------------------------------------
# Self test - every rule must fire on its own known-bad body and stay quiet on
# the safe spelling beside it, so a silent rule is a broken rule and not a
# clean codebase. Each case is scanned alone: a guard belonging to one sample
# must not suppress the next.
# --------------------------------------------------------------------------

SAMPLES: dict[str, tuple[str, str]] = {
    "c-string-call": (
        "void f(char* dst, const char* src) { strcpy(dst, src); }",
        "void f(std::string& dst, std::string_view src) { dst = std::format(\"{}\", src); }"),
    "stack-alloca": (
        "void f(size_t n) { void* p = alloca(n); }",
        "void f(size_t n) { df::blob b(n); }"),
    "raw-byte-move": (
        "void f(uint8_t* dst, const uint8_t* src, size_t n) { memcpy(dst, src, n); }",
        "void f(df::span dst, df::cspan src) { std::copy_n(src.data, src.size, dst.data); }"),
    "size-underflow": (
        "size_t f(const std::vector<int>& v) { return v.size() - 1; }",
        "size_t f(const std::vector<int>& v) { return v.empty() ? 0 : v.size() - 1; }"),
    "index-loop": (
        "void f(const std::vector<int>& v) { for (size_t i = 0; i < v.size(); ++i) { use(v[i]); } }",
        "void f(const std::vector<int>& v) { for (const auto& x : v) { use(x); } }"),
    "signed-index": (
        "void f(const std::vector<int>& v) { for (int i = 0; i < v.size(); ++i) { use(v[i]); } }",
        "void f(const std::vector<int>& v) { for (size_t i = 0; i < v.size(); ++i) { use(v[i]); } }"),
    "inclusive-bound": (
        "void f(const std::vector<int>& v) { for (size_t i = 0; i <= v.size(); ++i) { use(v[i]); } }",
        "void f(const std::vector<int>& v) { for (size_t i = 0; i < v.size(); ++i) { use(v[i]); } }"),
    "range-for-mutation": (
        "void f(std::vector<int>& v) { for (auto& x : v) { v.push_back(x); } }",
        "void f(std::vector<int>& v) { std::vector<int> n; for (auto& x : v) { n.push_back(x); } v = n; }"),
    "alias-then-resize": (
        "void f(std::vector<int>& v) { auto* p = v.data(); v.push_back(1); use(p); }",
        "void f(std::vector<int>& v) { v.push_back(1); auto* p = v.data(); use(p); }"),
    "dangling-view": (
        "void f() { std::string_view sv = make_name().c_str(); use(sv); }",
        "void f() { const auto name = make_name(); std::string_view sv = name; use(sv); }"),
    "return-inner-pointer": (
        "const char* f(const holder& h) { return h.name.c_str(); }",
        "std::string f(const holder& h) { return h.name; }"),
    "unchecked-element": (
        "int f(const std::vector<int>& v) { return v.front(); }",
        "int f(const std::vector<int>& v) { if (v.empty()) return 0; return v.front(); }"),
    "unchecked-find": (
        "void f(std::map<int, int>& m) { auto it = m.find(3); use(it->second); }",
        "void f(std::map<int, int>& m) { auto it = m.find(3); if (it != m.end()) use(it->second); }"),
    "use-after-move": (
        "void f(std::string s)\n{\n\tsink(std::move(s));\n\tuse(s.size());\n}",
        "void f(std::string s)\n{\n\tsink(std::move(s));\n\ts = other();\n\tuse(s.size());\n}"),
    "c-style-pointer-cast": (
        "void f(void* p) { use((uint32_t*)p); }",
        "void f(void* p) { use(static_cast<uint32_t*>(p)); }"),
    "pointer-type-pun": (
        "void f(uint8_t* base, size_t off) { auto* h = reinterpret_cast<header*>(base + off); }",
        "void f(const uint8_t* base, size_t off) { header h; std::memcpy(&h, base + off, sizeof(h)); }"),
    "raw-ownership": (
        "void f() { auto* p = new widget(); delete p; }",
        "void f() { auto p = std::make_unique<widget>(); }"),
    "default-capture-async": (
        "void f(state& s) { s.queue_async(async_queue::work, [&] { s.run(); }); }",
        "void f(state& s) { s.queue_async(async_queue::work, [&s] { s.run(); }); }"),
    "stack-buffer": (
        "void f()\n{\n\twchar_t path[MAX_PATH];\n\tuse(path);\n}",
        "void f()\n{\n\tstd::array<wchar_t, MAX_PATH> path;\n\tuse(path);\n}"),
}


def self_test() -> int:
    import tempfile

    def run(body: str) -> set[str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.cpp"
            path.write_text(body + "\n", encoding="utf-8")
            return {x.rule for x in scan([path], Path(tmp), None)}

    failures = []
    for rule_id in sorted(RULES):
        if rule_id not in SAMPLES:
            failures.append(f"{rule_id}: no self-test sample")
            continue
        unsafe, safe = SAMPLES[rule_id]
        if rule_id not in run(unsafe):
            failures.append(f"{rule_id}: did not fire on the unsafe sample")
        if rule_id in run(safe):
            failures.append(f"{rule_id}: fired on the safe sample")

    for line in failures:
        print(f"FAIL {line}")
    print(f"self test: {len(RULES)} rules, {len(failures)} problems")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("roots", nargs="*", default=["src"], type=Path)
    parser.add_argument("--exclude", action="append", default=[], help="regex matched against the posix path")
    parser.add_argument("--rule", action="append", default=[], help="restrict to these rule ids")
    parser.add_argument("--list-rules", action="store_true")
    parser.add_argument("--self-test", action="store_true", help="check every rule fires on a known-bad sample")
    parser.add_argument("--max-per-rule", type=int, default=40)
    parser.add_argument("--out", type=Path, default=Path("tmp/unsafe_memory.md"))
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.list_rules:
        for rule_id, (title, fix) in sorted(RULES.items()):
            print(f"{rule_id:24} {title}\n{'':24} -> {fix}\n")
        return 0

    roots = [Path(r) for r in (args.roots or ["src"])]
    files = collect_files(roots, args.exclude)
    if not files:
        print("no source files found")
        return 0

    rules = set(args.rule) or None
    findings = scan(files, roots[0], rules)
    write_report(findings, args.out, roots, len(files), args.max_per_rule)

    by_rule = Counter(x.rule for x in findings)
    severity = Counter(x.severity for x in findings)
    print(f"scanned {len(files)} files, {len(findings)} findings "
          f"(high {severity['high']}, medium {severity['medium']}, low {severity['low']})")
    for rule_id, count in sorted(by_rule.items(), key=lambda kv: -kv[1]):
        print(f"  {count:6}  {rule_id}")
    print(f"report: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
