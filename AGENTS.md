# Agents

Rules for AI assistants working in this repo.

- Commits are authored by the human. No `Co-Authored-By`, no tool signatures, no generated-with footers.
- Commit messages: one imperative line, a blank line, then only what a reader needs.
- Keep the tree lean: sources at the top, artifacts in `build/`, references in `plans/`. Both are ignored.
- Kernel side stays C and floor-only. Never add the cap command.
- Run `make test` before committing.
