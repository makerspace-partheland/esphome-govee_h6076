# Commit messages

Write commit titles and descriptions in English for this project.

# Instruction language

Maintain this project's `AGENTS.md` in English.

# Branches and version tags

- `main` contains the current stable code. Tested releases receive fixed Git tags using `release-YYYYMMDD`, for example `release-20261031`.
- `dev` is the development branch. Selected test versions from this branch receive fixed tags using `dev-YYYYMMDD`, for example `dev-20261031`.
- For additional releases of the same type on the same day, append `.1`, `.2`, and so on, for example `release-20261031.1` or `dev-20261031.1`.
- Do not move or reuse published tags. Assign a new tag to corrections.
- For a tagged revision, `VERSION` in `components/govee_h6076/__init__.py` must match the tag.
- Device templates continue to use `ref: main`, so regular firmware rebuilds use the current stable code, subject to ESPHome's cache refresh interval. A fixed release or development tag selects a specific revision; `ref: dev` follows the development branch.
- This convention alone does not authorize creating branches or tags, committing, pushing, or publishing.
