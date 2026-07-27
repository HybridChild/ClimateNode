#!/usr/bin/env bash
# Merge every compile_commands.json in the repo into one at the repo root, for clangd.
#
# Why this exists: clangd needs a compile command for each file it parses, and
# this repo produces *two* independent build trees:
#
#   firmware/build/compile_commands.json   the app       (scripts/build.sh)
#   twister-out/**/compile_commands.json   the tests     (scripts/test.sh)
#
# Pointing clangd at either one leaves the other's sources unparseable -- it
# falls back to a guessed command line with no Zephyr include paths, so
# <zephyr/ztest.h> "does not exist" and every symbol after it cascades into an
# error. Merging gives one database covering both, which .vscode/settings.json
# points clangd at via --compile-commands-dir=${workspaceFolder}.
#
# Run it by hand, after a first build and thereafter whenever the editor starts
# reporting missing headers in a file that compiles fine. That is rare: a compile
# database goes stale only when the *set of files or the flags* changes -- a new
# source file, a new test suite, a Kconfig or devicetree edit -- never from
# ordinary editing. Deliberately not wired into build.sh/test.sh, which stay thin
# `exec` wrappers; refreshing on every build would tax every build for a result
# that changes a few times a month.
#
# Harmless if only one of the two trees exists.
#
# The output is generated and .gitignore'd -- never edit it, and never rely on it
# for anything but the editor. The build reads the per-tree files, not this one.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

python3 - "$REPO" <<'PY'
import json, pathlib, sys

repo = pathlib.Path(sys.argv[1])

# Firmware first: the app and the tests both compile large parts of Zephyr
# itself (kernel/, subsys/) but with different Kconfig, so the same source
# appears in both databases with different flags. First entry per file wins,
# and for shared Zephyr sources the firmware's configuration is the one that
# actually ships -- so that is the one worth reading the code under.
sources = [repo / "firmware" / "build" / "compile_commands.json"]
sources += sorted((repo / "twister-out").glob("**/compile_commands.json"))

merged, seen = [], set()
for db in sources:
    if not db.is_file():
        continue
    try:
        entries = json.loads(db.read_text())
    except json.JSONDecodeError:
        print(f"ide-index: skipping malformed {db}", file=sys.stderr)
        continue
    for entry in entries:
        # A build tree that was deleted (twister --clobber-output) can leave
        # entries whose source file is gone; clangd does not need them.
        path = entry.get("file")
        if not path or path in seen:
            continue
        seen.add(path)
        merged.append(entry)

if not merged:
    print("ide-index: no compile_commands.json found -- run scripts/build.sh "
          "or scripts/test.sh first", file=sys.stderr)
    raise SystemExit(1)

out = repo / "compile_commands.json"
out.write_text(json.dumps(merged, indent=1) + "\n")
print(f"ide-index: {len(merged)} entries from {len(seen)} files -> {out.relative_to(repo)}")
PY
