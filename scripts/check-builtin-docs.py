#!/usr/bin/env python3
"""Fail when the builtin reference and the registered builtins disagree.

Every name registered with register_unary / register_binary / register_vary
(and the *_op variants) in src/ must appear in the "Registered Builtin
Surface" section of docs/docs/reference/all-functions.md, and every name in
that section must still be registered.  The categorized tables below it are
the readable part; this section is the complete list the page promises.

Usage: python3 scripts/check-builtin-docs.py [--page FILE]   (or: make docs-check)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PAGE = ROOT / "docs/docs/reference/all-functions.md"
REGISTER = re.compile(
    r'register_(?:binary_op|unary_op|binary|unary|vary)\s*\(\s*"([^"]+)"')


def registered():
    names = set()
    for path in sorted((ROOT / "src").rglob("*.c")):
        names.update(REGISTER.findall(path.read_text(errors="ignore")))
    return names


def surface(page):
    lines = page.read_text().splitlines()
    start = next(i for i, l in enumerate(lines)
                 if l.startswith("## Registered Builtin Surface"))
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith("## ")), len(lines))
    names = set(re.findall(r"`([^`]+)`", "\n".join(lines[start:end])))
    # The section's own prose quotes file names; they are not builtins.
    return {n for n in names
            if not n.startswith("src/") and not n.endswith((".py", ".md", ".c"))}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--page", type=Path, default=DEFAULT_PAGE)
    args = parser.parse_args()
    reg, doc = registered(), surface(args.page)
    missing, stale = sorted(reg - doc), sorted(doc - reg)
    if missing:
        print("registered but missing from the Registered Builtin Surface:")
        print("  " + " ".join(missing))
    if stale:
        print("listed in the Registered Builtin Surface but not registered:")
        print("  " + " ".join(stale))
    if missing or stale:
        print("\nAdd each builtin to its arity list there, give it a row in the "
              "matching category table, and document it "
              "(CONTRIBUTING.md, 'Documentation').")
        return 1
    print(f"builtin docs: {len(reg)} registered builtins, all listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
