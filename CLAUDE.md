# Agent instructions

All rules for this repository are in **[AGENTS.md](AGENTS.md)** - one file, one
place to update, no per-tool copies.

`python3 scripts/check_repo_rules.py` enforces the mechanical half in CI
(`.github/workflows/hygiene.yml`, on every push); `--fix` repairs the encoding and
line-ending violations. Build workflows only run when a build input changes, so a
documentation edit costs you nothing but that one job.
