# AGENTS.md

This repository uses `CLAUDE.md` as the primary behavioral and execution policy for coding tasks.

## Priority
- Follow `CLAUDE.md` first for implementation behavior, scope control, and verification style.
- For structural code questions, prefer CodeGraph-first workflow as documented in `CLAUDE.md`.
- If any local instruction files conflict, prefer the stricter rule unless the user explicitly overrides it.

## Maintenance
- Keep detailed policy in `CLAUDE.md`.
- Keep this file minimal to avoid drift.
