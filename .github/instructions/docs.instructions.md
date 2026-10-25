---
description: Single-owner rules for the documentation set, and what each document may not become.
applyTo: 'docs/**/*.md, AGENTS.md, README.md'
---

# Editing the documentation

The ownership table in [AGENTS.md](../../AGENTS.md) is the index. Every subject has exactly one
owning document.

## Rules

- **Move information to its owner; link rather than duplicate.** Duplicated volatile detail is how
  two documents come to disagree, and an agent reading the wrong one cannot tell.
- **GitHub issues and releases own work, status, discussion, follow-up, and release history.**
  Repository documents describe current behavior and constraints, not how they evolved.
- **Source owns exact APIs, enums, signatures and file lists.** Documents name files and stable
  symbols so a reader can route; they do not restate a signature that the compiler already checks.
- Unfinished ideas, alternatives, open questions, validation transcripts, and rejected designs do
  not belong in the documentation set.

## Where this lives

Each document ends with a **Where this lives** section naming the source that implements it. Keep
it to files and stable symbols. When you move or rename a file, update every document that names
it — `tools/lint_repo.ps1` fails on a `src/` path that no longer exists, on a link that does not
resolve, on a `#heading` anchor that is not a heading, and on a document with no owner row.

## Before adding a new document

Prefer a section in the existing owner. A new document needs a row in the AGENTS.md ownership
table stating what it owns, and that claim must not overlap an existing row. If you cannot state
the boundary in one line, the content belongs in an existing document.

## Check

`pwsh -File tools/lint_repo.ps1` — a few seconds, and it is the only thing standing between this
set and quiet rot.
