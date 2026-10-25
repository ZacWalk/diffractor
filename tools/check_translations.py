"""Validate Diffractor translations end to end.

This runs two phases:

  * **Phase 1 - app_text registration** (formerly ``check_app_text.py``): every
    ``text_t`` / ``plural_text`` member declared in ``src/app_text.h``, including
    its ``platform_text`` messages and ``words`` from libdesktop, must be
    registered exactly once, in the correct vector, in ``src/app_text.cpp``. A
    broken registration is always fatal.
  * **Phase 2 - translation (.po) consistency**: read all of
    ``exe/languages/*.po`` and build a per-string view across every language so
    the translations can be reviewed for consistency. It is meant to give a
    human (or an agent) an easy way to spot:

  * untranslated / absent strings - a source string that exists in the union of
    all ``.po`` files but is empty or missing in one or more languages;
  * placeholder mismatches - anonymous ``{}`` are substituted positionally, so
    the *count* must match the source exactly: dropping one shifts every later
    value and leaves the last one unfilled. Named tokens such as ``{count}`` or
    ``{first-name}`` are substituted by name, so only their presence is checked
    and repeating one is legitimate. (The *order* of anonymous ``{}`` cannot be
    validated - they are textually identical, so a translation that swaps two
    values is indistinguishable from a correct one. That stays a review
    question.)
  * accelerator mismatches - a ``&`` that begins a word in the English source is
    a Win32 mnemonic marker (``&OK``, ``As &JPEG``), because dialog buttons are
    real Win32 controls. A translation may move the marker to a different
    letter, but must keep exactly one and must not leave it dangling. A ``&``
    surrounded by spaces or inside a word is literal text (``Fitness &
    Workout``, ``R&B``) and translations may render it as the local "and".
  * quality mismatches in existing translations - objective, language-agnostic
    structural differences between a source string and its translation: leading
    or trailing whitespace (matters because several strings are concatenated in
    the UI), embedded newline count, a dropped trailing colon, or a dropped
    sentence-ending ``.``/``?``/``!`` (the last is skipped for CJK text, where
    omitting the period on labels is the normal convention);
    * extra plural-form tokens - Slavic languages (Czech, Polish, Russian,
        Ukrainian) declare additional forms at index 2 and above (``msgstr[2]``,
        ``msgstr[3]``). ``load_po`` parses them and the app selects them at runtime
        for the relevant counts, so the report only flags an extra form whose
        placeholder tokens are not present in the singular or plural source.

The English source text *is* the ``msgid`` in each ``.po`` file (Diffractor's
``gen_po`` test generates the msgids from the in-code strings), so the union of
all msgids is used as the reference set of source strings.

Usage (from the repo root, using the project virtual environment)::

    tools\\.venv\\Scripts\\python.exe tools\\check_translations.py

A full Markdown report grouped by source string is written to
``tmp/translation_report.md`` and a summary plus the list of problems is printed
to stdout. Pass ``--full`` to also dump every string to stdout.

Exit code: non-zero when phase 1 finds a registration error or phase 2 finds a
placeholder or accelerator mismatch (all real bugs). Untranslated, quality and
extra plural-form issues are reported as warnings and only fail the run under
``--strict`` (many languages are intentionally incomplete).

The parser is intentionally self-contained (no third-party dependency) and
tolerant in the same way Diffractor's own ``load_po`` is - e.g. it accepts the
stray trailing semicolons present in some ``.po`` files.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LANG_DIR = REPO_ROOT / "exe" / "languages"
DEFAULT_REPORT = REPO_ROOT / "tmp" / "translation_report.md"

# Source files for the app_text registration check (phase 1).
APP_TEXT_HEADER = REPO_ROOT / "src" / "app_text.h"
APP_TEXT_IMPL = REPO_ROOT / "src" / "app_text.cpp"

# Runtime placeholders look like {count}, {first-name}, {total}, {} ...
TOKEN_RE = re.compile(r"\{[^{}]*\}")

# Mirrors language_name() in src/app_text.cpp.
LANGUAGE_NAMES = {
    "br": "Breton",
    "cs": "Czech",
    "de": "German",
    "en": "English",
    "es": "Spanish",
    "fr": "French",
    "it": "Italian",
    "ja": "Japanese",
    "ko": "Korean",
    "lv": "Latvian",
    "nl": "Dutch",
    "pl": "Polish",
    "pt": "Portuguese",
    "ru": "Russian",
    "sr": "Serbian",
    "tr": "Turkish",
    "uk": "Ukrainian",
    "zh": "Chinese",
}


def language_name(code: str) -> str:
    return LANGUAGE_NAMES.get(code, code)


def tokens(text: str) -> list[str]:
    """Return the sorted, de-duplicated set of {placeholder} tokens in text."""
    return sorted(set(TOKEN_RE.findall(text)))


def _split_tokens(text: str) -> tuple[int, list[str]]:
    """Split placeholders into the anonymous count and the named token list."""
    anonymous = 0
    named: list[str] = []
    for token in TOKEN_RE.findall(text):
        if token == "{}":
            anonymous += 1
        else:
            named.append(token)
    return anonymous, named


def placeholder_issues(src: str, got: str) -> list[str]:
    """Placeholder defects that break substitution at runtime.

    Anonymous ``{}`` are filled positionally, so the *count* must match exactly:
    a translation that drops one shifts every later value and leaves the last
    ``{}`` unfilled. Named tokens such as ``{count}`` are filled by name, so only
    their presence matters and repeating one is legitimate.

    The order of anonymous ``{}`` cannot be validated here - they are textually
    identical, so a translation that swaps two values is indistinguishable from
    a correct one. That remains a review question, not a check.
    """
    issues: list[str] = []

    src_anonymous, src_named = _split_tokens(src)
    got_anonymous, got_named = _split_tokens(got)

    if src_anonymous != got_anonymous:
        issues.append(
            f"placeholder count mismatch: {src_anonymous} x '{{}}' in source, {got_anonymous} in translation"
        )

    src_names, got_names = sorted(set(src_named)), sorted(set(got_named))
    if src_names != got_names:
        issues.append(f"placeholder token mismatch: src {src_names} != {got_names}")

    return issues


# A '&' that begins a word in the English source is a Win32 mnemonic marker
# (&OK, As &JPEG, &Don't Save): the dialog buttons created by create_button are
# real Win32 controls, so Alt+letter activates them. A '&' that is surrounded by
# spaces or sits inside a word is literal text (Fitness & Workout, R&B) and a
# translation may legitimately render it as the local word for "and".
MNEMONIC_RE = re.compile(r"(?:^|(?<=\s))&(?=\w)")


def accelerator_issues(src: str, got: str) -> list[str]:
    """Mnemonic ``&`` markers dropped, duplicated or left dangling.

    Only checked when the English source carries a mnemonic. A translation may
    move the marker to a different letter - that is the translator's job - but it
    must keep exactly one, and it must sit before a character.

    Not checked: whether two commands in the same menu claim the same letter.
    That needs the menu grouping, which is not visible in a .po file.
    """
    issues: list[str] = []

    expected = len(MNEMONIC_RE.findall(src))
    if expected == 0:
        return issues

    found = got.count("&")
    if found != expected:
        issues.append(f"accelerator mismatch: {expected} mnemonic '&' in source, {found} in translation")
    elif re.search(r"&(\s|$)", got):
        issues.append("accelerator marker '&' does not precede a character")

    return issues


# Phrase-terminating punctuation, ASCII plus the full-width / CJK forms used by
# Japanese so the check does not flag legitimate Japanese punctuation.
_TERMINAL_MAP = {
    "。": ".", "．": ".", "：": ":", "？": "?", "！": "!", "…": "…", "‥": "…",
    ".": ".", ":": ":", "?": "?", "!": "!",
}


def _terminal(text: str) -> str:
    """Return the normalised trailing terminal punctuation of text, or ''."""
    text = text.rstrip()
    if not text:
        return ""
    return _TERMINAL_MAP.get(text[-1], "")


def _has_cjk(text: str) -> bool:
    """True if text contains Japanese/Chinese script.

    Japanese UI text conventionally omits the sentence-ending period on short
    labels and tooltips, so the dropped-period check is skipped for CJK to avoid
    flagging that deliberate, consistent style.
    """
    return any(
        "\u3040" <= ch <= "\u30ff"  # hiragana + katakana
        or "\u3400" <= ch <= "\u9fff"  # CJK ideographs
        or "\uff66" <= ch <= "\uff9f"  # half-width katakana
        for ch in text
    )


def quality_issues(src: str, got: str) -> list[str]:
    """Objective source-vs-translation mismatches in an existing translation.

    These are structural (not fluency) checks that are safe to flag without
    knowing the language: leading/trailing whitespace, embedded newline count,
    and presence of trailing terminal punctuation (``.``/``:``/``?``/``!``,
    CJK-aware). They catch dropped colons, broken concatenation spacing and
    truncated sentences.
    """
    issues: list[str] = []
    if not got.strip():
        return issues  # empty is reported separately as "untranslated"

    if src.startswith((" ", "\t")) != got.startswith((" ", "\t")):
        issues.append("leading-space mismatch")
    src_space = src.endswith((" ", "\t"))
    got_space = got.endswith((" ", "\t"))
    # CJK text conventionally omits ASCII separator spaces, so a CJK translation
    # dropping the source's trailing separator space is a deliberate, correct
    # typographic choice - not a defect. A translation that ADDS an unexpected
    # trailing space is still flagged.
    if src_space != got_space and not (src_space and not got_space and _has_cjk(got)):
        issues.append("trailing-space mismatch")
    if src.count("\n") != got.count("\n"):
        issues.append(f"newline mismatch (src {src.count(chr(10))} / got {got.count(chr(10))})")

    s_term, g_term = _terminal(src), _terminal(got)
    # A trailing colon is a structural label marker - flag if either side has it
    # but the other does not. Sentence enders (. ? !) are only flagged when the
    # source has one but a non-CJK translation dropped all terminal punctuation.
    if (s_term == ":") != (g_term == ":"):
        issues.append(f"colon mismatch (src {s_term!r} / got {g_term!r})")
    elif s_term in (".", "?", "!") and g_term == "" and not _has_cjk(got):
        issues.append(f"end-punctuation dropped (src {s_term!r})")

    return issues


def display(text: str) -> str:
    """Collapse a translation onto a single readable line for the report."""
    return text.replace("\r\n", "\n").replace("\n", "\\n").strip()


def un_escape(line: str) -> str:
    """Extract and un-escape the quoted value from a .po line.

    Mirrors ``un_escape`` in src/app_text.cpp: take the text between the first
    and last double quote, then resolve ``\\n``, ``\\"`` and ``\\\\``. This is
    deliberately lenient about anything outside the quotes (e.g. a stray
    trailing ``;``).
    """
    start = line.find('"')
    end = line.rfind('"')
    if start == -1 or end == -1 or start >= end:
        return ""
    result = line[start + 1:end]
    result = result.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")
    return result


def _new_entry() -> dict:
    return {"id": "", "id_plural": "", "str": "", "str_plural": "", "str_extra": ""}


def _is_empty(entry: dict) -> bool:
    return not any(entry.values())


def parse_po(path: Path) -> list[dict]:
    """Parse a .po file into raw entries, mirroring Diffractor's load_po state
    machine but keeping forms at index 2 and above in ``str_extra`` so the
    report can distinguish them from the two forms Diffractor loads.
    """
    entries: list[dict] = []
    entry = _new_entry()
    state = None

    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip(" \t\r\n")
            if not line or line[0] == "#":
                continue

            if line.startswith("msgstr[1]"):
                state = "str_plural"
            elif line.startswith("msgstr[0]"):
                state = "str"
            elif line.startswith("msgstr["):
                state = "str_extra"
            elif line.startswith("msgid_plural"):
                state = "id_plural"
            elif line.startswith("msgstr"):
                state = "str"
            elif line.startswith("msgid"):
                state = "id"
            # otherwise this is a continuation line ("...") and keeps the state

            if state == "id" and not _is_empty(entry) and line[0] != '"':
                entries.append(entry)
                entry = _new_entry()

            value = un_escape(line)
            if state is not None:
                entry[state] += value

    if not _is_empty(entry):
        entries.append(entry)

    return entries


def load_po_file(path: Path) -> "OrderedDict[str, dict]":
    """Parse one .po file into an ordered map: msgid -> entry data.

    ``msgstr`` is the singular (``msgstr`` or ``msgstr[0]``), ``msgstr_plural``
    is ``msgstr[1]``, and ``msgstr_extra`` holds the higher indexes (the extra
    Slavic forms), which are selected at runtime for the relevant counts.
    """
    out: "OrderedDict[str, dict]" = OrderedDict()
    for entry in parse_po(path):
        if entry["id"] == "":
            continue  # skip the header entry
        out[entry["id"]] = {
            "singular": entry["id"],
            "plural": entry["id_plural"],
            "msgstr": entry["str"],
            "msgstr_plural": entry["str_plural"],
            "msgstr_extra": entry["str_extra"],
        }
    return out


def analyse(languages: "OrderedDict[str, OrderedDict]"):
    """Build the reference source set and find per-string problems.

    Returns a tuple ``(source, problems)`` where ``source`` maps each msgid to
    its English singular/plural text and ``problems`` is a list of
    ``(msgid, {lang_code: [issue, ...]})`` for every string with at least one
    issue in at least one language.
    """
    source: "OrderedDict[str, dict]" = OrderedDict()
    for entries in languages.values():
        for msgid, data in entries.items():
            if msgid not in source:
                source[msgid] = {"singular": data["singular"], "plural": data["plural"]}

    problems = []

    for msgid, src in source.items():
        src_singular_tokens = tokens(src["singular"])
        src_plural_tokens = tokens(src["plural"]) if src["plural"] else []
        row_issues: "OrderedDict[str, list[str]]" = OrderedDict()

        for code, entries in languages.items():
            data = entries.get(msgid)
            issues: list[str] = []

            if data is None:
                issues.append("absent")
            else:
                if not data["msgstr"].strip():
                    issues.append("untranslated")
                else:
                    issues.extend(placeholder_issues(src["singular"], data["msgstr"]))
                    issues.extend(accelerator_issues(src["singular"], data["msgstr"]))
                    issues.extend(quality_issues(src["singular"], data["msgstr"]))

                if src["plural"]:
                    if not data["msgstr_plural"].strip():
                        issues.append("plural untranslated")
                    else:
                        issues.extend(
                            f"plural {i}" for i in placeholder_issues(src["plural"], data["msgstr_plural"])
                        )
                        issues.extend(
                            f"plural {i}" for i in accelerator_issues(src["plural"], data["msgstr_plural"])
                        )
                        issues.extend(
                            f"plural {i}" for i in quality_issues(src["plural"], data["msgstr_plural"])
                        )

                if data["msgstr_extra"].strip():
                    # Extra Slavic plural forms (msgstr[2..]) are used at runtime.
                    # They legitimately draw tokens from the singular or plural
                    # source; only an unexpected token is a real defect.
                    got_extra = tokens(data["msgstr_extra"])
                    allowed = set(src_singular_tokens) | set(src_plural_tokens)
                    unexpected = sorted({t for t in got_extra if t not in allowed})
                    if unexpected:
                        issues.append(
                            f"extra plural token mismatch: {unexpected} not in {sorted(allowed)}"
                        )

            if issues:
                row_issues[code] = issues

        if row_issues:
            problems.append((msgid, row_issues))

    return source, problems


def compute_stats(languages, source, problems):
    """Per-language coverage and issue counts keyed by language code."""
    problem_map = {msgid: ri for msgid, ri in problems}
    stats: "OrderedDict[str, dict]" = OrderedDict()

    for code, entries in languages.items():
        total = len(source)
        translated = sum(1 for m in source if m in entries and entries[m]["msgstr"].strip())
        token_issues = sum(
            1
            for ri in problem_map.values()
            if code in ri and any("placeholder" in issue for issue in ri[code])
        )
        accelerator = sum(
            1
            for ri in problem_map.values()
            if code in ri and any("accelerator" in issue for issue in ri[code])
        )
        extra_issues = sum(
            1
            for ri in problem_map.values()
            if code in ri and any("extra plural" in issue for issue in ri[code])
        )
        quality = sum(
            1
            for ri in problem_map.values()
            if code in ri and any(
                ("space" in issue or "newline" in issue or "punctuation" in issue
                 or "colon" in issue)
                for issue in ri[code]
            )
        )
        stats[code] = {
            "total": total,
            "translated": translated,
            "missing": total - translated,
            "token_issues": token_issues,
            "accelerator": accelerator,
            "quality": quality,
            "extra_plurals": extra_issues,
        }

    return stats


def format_string_block(msgid, source, languages, row_issues=None):
    """Render one source string with every language's translation."""
    src = source[msgid]
    lines = [f"- **en**: `{display(src['singular'])}`"]
    if src["plural"]:
        lines.append(f"    - en *(plural)*: `{display(src['plural'])}`")

    for code, entries in languages.items():
        data = entries.get(msgid)
        if data is None:
            lines.append(f"    - **{code}**: _(absent)_")
            continue

        value = display(data["msgstr"]) if data["msgstr"].strip() else "_(untranslated)_"
        lines.append(f"    - **{code}**: {value}")
        if src["plural"]:
            plural_value = (
                display(data["msgstr_plural"]) if data["msgstr_plural"].strip() else "_(untranslated)_"
            )
            lines.append(f"        - plural: {plural_value}")

        if row_issues and code in row_issues:
            lines.append(f"        - :warning: {'; '.join(row_issues[code])}")

    return "\n".join(lines)


def write_report(report_path, languages, source, stats, problems):
    lines: list[str] = []
    lines.append("# Diffractor translation consistency report")
    lines.append("")
    lines.append(f"Languages: {', '.join(f'{c} ({language_name(c)})' for c in languages)}")
    lines.append(f"Source strings (union of all msgids): **{len(source)}**")
    lines.append("")

    # Notes for reviewers
    lines.append("## Notes for reviewers")
    lines.append("")
    lines.append(
        "- **`//` disambiguation operator**: a msgid may contain a `//` marker, "
        "for example `Country//genre`. Everything **before** `//` is the text "
        "shown to the user (`Country`); everything **after** `//` is "
        "disambiguation context for translators (`genre`) and is **stripped at "
        "runtime** by `tt_prep` in `src/app_text.cpp`. This lets two identical "
        "display strings (e.g. the *Country* location field vs. the *Country* "
        "music genre) exist as distinct msgids with separate translations. When "
        "translating, translate only the part before `//` and keep the `//...` "
        "suffix unchanged in the msgid."
    )
    lines.append(
        "- **Plural forms**: `msgstr[0]` is the singular, `msgstr[1]` the general "
        "plural, and `msgstr[2]`/`msgstr[3]` the extra Slavic forms (Czech, "
        "Polish, Russian, Ukrainian). The extra forms are selected at runtime "
        "for the relevant counts; other languages use only `msgstr[0]`/`msgstr[1]`."
    )
    lines.append(
        "- **What the checks cannot see**: the *order* of anonymous `{}` "
        "placeholders is not verifiable - they are textually identical, so a "
        "translation that renders a date where the count belongs looks correct "
        "to the tool. Likewise, two commands in the same menu claiming the same "
        "`&` mnemonic letter needs the menu grouping, which a `.po` file does "
        "not carry. Both remain review questions."
    )
    lines.append("")

    # Summary table
    lines.append("## Coverage summary")
    lines.append("")
    lines.append("| Code | Language | Source | Translated | Missing | Placeholders | Accelerators | Quality | Extra plurals |")
    lines.append("|------|----------|-------:|-----------:|--------:|-------------:|-------------:|--------:|----------:|")
    for code, s in stats.items():
        lines.append(
            f"| {code} | {language_name(code)} | {s['total']} | {s['translated']} "
            f"| {s['missing']} | {s['token_issues']} | {s['accelerator']} | {s['quality']} | {s['extra_plurals']} |"
        )
    lines.append("")

    # Problems
    lines.append(f"## Problems ({len(problems)} strings)")
    lines.append("")
    if not problems:
        lines.append("No missing translations, placeholder or accelerator mismatches or extra plural forms found.")
    else:
        lines.append(
            "Each entry below has a missing translation, a placeholder "
            "mismatch, an accelerator (`&`) mismatch, a structural quality "
            "mismatch (whitespace / newline / punctuation) or an extra plural "
            "form in at least one language."
        )
        lines.append("")
        for msgid, row_issues in problems:
            lines.append(format_string_block(msgid, source, languages, row_issues))
            lines.append("")
    lines.append("")

    # Full dump
    lines.append("## All strings")
    lines.append("")
    for msgid in source:
        lines.append(format_string_block(msgid, source, languages))
        lines.append("")

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines), encoding="utf-8")


def print_summary(languages, source, stats, problems, report_path):
    print(f"Parsed {len(languages)} languages from {DEFAULT_LANG_DIR}")
    print(f"Source strings (union of all msgids): {len(source)}")
    print()
    header = (
        f"{'Code':<5} {'Language':<12} {'Source':>7} {'Translated':>11} "
        f"{'Missing':>8} {'Holders':>8} {'Accel':>6} {'Quality':>8} {'Extra':>10}"
    )
    print(header)
    print("-" * len(header))
    for code, s in stats.items():
        print(
            f"{code:<5} {language_name(code):<12} {s['total']:>7} {s['translated']:>11} "
            f"{s['missing']:>8} {s['token_issues']:>8} {s['accelerator']:>6} "
            f"{s['quality']:>8} {s['extra_plurals']:>10}"
        )
    print()
    print(f"Strings with at least one problem: {len(problems)}")
    print(f"Full report written to: {report_path}")


def print_full(languages, source):
    for msgid in source:
        print(format_string_block(msgid, source, languages))
        print()


# ---------------------------------------------------------------------------
# Phase 1: app_text registration integrity (formerly check_app_text.py)
#
# Verify that every ``text_t`` / ``plural_text`` member declared in app_text.h
# is registered exactly once, in the correct vector, in app_text.cpp. A broken
# registration is a hard build/runtime bug, so these are always fatal.
# ---------------------------------------------------------------------------

_DECLARATION_RE = re.compile(
    r"^\s*(text_t|plural_text)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\{)",
    re.MULTILINE,
)


def _app_text_declarations(source: str) -> dict:
    result = {"text_t": set(), "plural_text": set()}
    for type_name, member_name in _DECLARATION_RE.findall(source):
        result[type_name].add(member_name)
    return result


def _app_text_registrations(source: str, vector_name: str) -> list:
    vector_re = re.compile(
        rf"{re.escape(vector_name)}\s*=\s*"
        r"std::vector<std::reference_wrapper<(?:text_t|plural_text)>>\s*"
        r"\{(?P<body>.*?)^\s*\};",
        re.MULTILINE | re.DOTALL,
    )
    match = vector_re.search(source)
    if match is None:
        raise ValueError(f"Could not find the {vector_name} initializer")

    names: list = []
    for raw_line in match.group("body").splitlines():
        line = raw_line.split("//", 1)[0].strip().rstrip(",")
        if not line:
            continue
        if not re.fullmatch(r"(?:(?:platform_text|words)\.)?[A-Za-z_][A-Za-z0-9_]*", line):
            raise ValueError(f"Unexpected entry in {vector_name}: {raw_line.strip()}")
        names.append(line)
    return names


def _first_duplicates(names: list) -> set:
    seen: set = set()
    return {name for name in names if name in seen or seen.add(name)}


def _platform_text_header() -> Path:
    sibling = REPO_ROOT.parent / "libdesktop" / "include" / "libdesktop" / "platform.h"
    if sibling.is_file():
        return sibling
    fetched = sorted(REPO_ROOT.glob("tmp/*/_deps/libdesktop-src/include/libdesktop/platform.h"))
    signatures = {
        (path.read_bytes(), path.with_name("util_strings.h").read_bytes())
        for path in fetched
    }
    if fetched and len(signatures) == 1:
        return fetched[0]
    raise ValueError(
        "Cannot identify libdesktop's platform.h. Configure the build first or "
        "pass --platform-text-header with the header used by this build."
    )


def check_app_text_registration(platform_header: Path | None = None) -> list:
    """Return a list of human-readable error lines; empty means everything is OK."""
    header = APP_TEXT_HEADER.read_text(encoding="utf-8")
    declared = _app_text_declarations(header)
    implementation = APP_TEXT_IMPL.read_text(encoding="utf-8")

    try:
        for declaration, member, filename in (
            ("platform::messages", "platform_text", "platform.h"),
            ("str::words", "words", "util_strings.h"),
        ):
            if re.search(rf"^\s*{re.escape(declaration)}\s+{member}\s*;", header, re.MULTILINE):
                shared = platform_header if platform_header is not None else _platform_text_header()
                if filename != "platform.h":
                    shared = shared.with_name(filename)
                messages = _app_text_declarations(shared.read_text(encoding="utf-8"))
                if not messages["text_t"]:
                    raise ValueError(f"No platform messages found in {shared}")
                for kind, names in messages.items():
                    declared[kind].update(f"{member}.{name}" for name in names)
        text_entries = _app_text_registrations(implementation, "_all_texts")
        plural_entries = _app_text_registrations(implementation, "_all_plurals")
    except (OSError, ValueError) as error:
        return [f"ERROR: {error}"]

    registered_texts = set(text_entries)
    registered_plurals = set(plural_entries)

    problems: list = []

    def add(label: str, names: set) -> None:
        if names:
            problems.append(f"{label} ({len(names)}): " + ", ".join(sorted(names)))

    add("Missing from _all_texts", declared["text_t"] - registered_texts)
    add("Missing from _all_plurals", declared["plural_text"] - registered_plurals)
    add("Unknown entries in _all_texts", registered_texts - declared["text_t"])
    add("Unknown entries in _all_plurals", registered_plurals - declared["plural_text"])
    add("text_t members registered as plurals", declared["text_t"] & registered_plurals)
    add("plural_text members registered as singulars", declared["plural_text"] & registered_texts)
    add("Duplicate entries in _all_texts", _first_duplicates(text_entries))
    add("Duplicate entries in _all_plurals", _first_duplicates(plural_entries))

    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--platform-text-header", type=Path,
        help="libdesktop/include/libdesktop/platform.h used by the build; otherwise "
             "use the sibling checkout or an unambiguous fetched copy",
    )
    parser.add_argument(
        "--lang-dir", type=Path, default=DEFAULT_LANG_DIR,
        help="folder containing the .po files (default: exe/languages)",
    )
    parser.add_argument(
        "--report", type=Path, default=DEFAULT_REPORT,
        help="path for the Markdown report (default: tmp/translation_report.md)",
    )
    parser.add_argument(
        "--full", action="store_true",
        help="also print the full per-string comparison to stdout",
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="also fail on warnings (untranslated, quality and extra plural forms), "
             "not just on registration errors and token mismatches",
    )
    args = parser.parse_args(argv)

    # Phase 1: app_text registration integrity (always fatal on error).
    print("== Phase 1: app_text registration ==")
    app_text_errors = check_app_text_registration(args.platform_text_header)
    for line in app_text_errors:
        print(f"  {line}", file=sys.stderr)
    if not app_text_errors:
        print("  app_text registration OK")
    print()

    # Phase 2: translation (.po) consistency.
    print("== Phase 2: translation (.po) consistency ==")
    po_paths = sorted(args.lang_dir.glob("*.po"))
    if not po_paths:
        print(f"No .po files found in {args.lang_dir}", file=sys.stderr)
        return 1

    languages = OrderedDict((p.stem, load_po_file(p)) for p in po_paths)
    source, problems = analyse(languages)
    stats = compute_stats(languages, source, problems)

    write_report(args.report, languages, source, stats, problems)
    print_summary(languages, source, stats, problems, args.report)
    if args.full:
        print()
        print_full(languages, source)

    # Aggregate failures. Placeholder and accelerator mismatches are real runtime
    # bugs and always fail. Registration errors always fail. Untranslated, quality
    # and extra-plural issues are expected incompleteness and only fail under --strict.
    placeholder_failures = sum(s["token_issues"] for s in stats.values())
    accelerator_failures = sum(s["accelerator"] for s in stats.values())
    quality_failures = sum(
        s["missing"] + s["quality"] + s["extra_plurals"] for s in stats.values()
    )

    hard_fail = bool(app_text_errors) or placeholder_failures > 0 or accelerator_failures > 0
    if args.strict:
        hard_fail = hard_fail or quality_failures > 0

    print()
    if hard_fail:
        reasons = []
        if app_text_errors:
            reasons.append(f"{len(app_text_errors)} app_text registration error(s)")
        if placeholder_failures:
            reasons.append(f"{placeholder_failures} placeholder mismatch(es)")
        if accelerator_failures:
            reasons.append(f"{accelerator_failures} accelerator mismatch(es)")
        if args.strict and quality_failures:
            reasons.append(f"{quality_failures} warning(s) (strict)")
        print(f"FAILED: {', '.join(reasons)}", file=sys.stderr)
        return 1

    print("Translation validation OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
