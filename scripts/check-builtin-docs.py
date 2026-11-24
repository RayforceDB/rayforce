#!/usr/bin/env python3
"""Fail when the builtin reference and the registered builtins disagree.

Every name registered with register_unary / register_binary / register_vary
(and the *_op variants) in src/ must appear in the matching subsection of the
"Registered Builtin Surface" section of docs/docs/reference/all-functions.md
(### Unary, ### Binary, ### Variadic / Special), and every name listed there
must still be registered under that arity.  The categorized tables below it
are the readable part; this section is the complete list the page promises.

Checks names and arities.  It reads the source text, so a registration that is
compiled out on a platform or built by a macro is counted as the text reads.

Usage: python3 scripts/check-builtin-docs.py [--page FILE]   (or: make docs-check)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PAGE = ROOT / "docs/docs/reference/all-functions.md"
REGISTER = re.compile(
    r'register_(binary_op|unary_op|binary|unary|vary)\s*\(\s*"([^"]+)"')
KIND = {"unary": "Unary", "unary_op": "Unary",
        "binary": "Binary", "binary_op": "Binary",
        "vary": "Variadic / Special"}
# Registered unary, but the evaluator promotes them to a two-argument form
# when called with two arguments (see the unary-to-binary mapping in
# src/lang/eval.c), so they are also listed as Binary.
ALSO_BINARY = ("min", "max")
ARITIES = ("Unary", "Binary", "Variadic / Special")


def registered():
    """The set of (name, arity) pairs registered in src/."""
    pairs = set()
    for path in sorted((ROOT / "src").rglob("*.c")):
        for kind, name in REGISTER.findall(path.read_text(errors="ignore")):
            pairs.add((name, KIND[kind]))
    pairs.update((n, "Binary") for n in ALSO_BINARY if (n, "Unary") in pairs)
    return pairs


def surface(page):
    """The set of (name, arity) pairs listed, one ### subsection per arity."""
    lines = page.read_text().splitlines()
    start = next(i for i, l in enumerate(lines)
                 if l.startswith("## Registered Builtin Surface"))
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith("## ")), len(lines))
    pairs, arity = set(), None
    for line in lines[start:end]:
        if line.startswith("### "):
            arity = line[4:].strip()
            if arity not in ARITIES:
                sys.exit(f"unknown subsection in the surface list: {line}")
        elif arity is not None:
            for n in re.findall(r"`([^`]+)`", line):
                pairs.add((n, arity))
    return pairs


def show(pairs):
    return "  " + " ".join(f"{n} ({a})" for n, a in sorted(pairs))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--page", type=Path, default=DEFAULT_PAGE)
    args = parser.parse_args()
    reg, doc = registered(), surface(args.page)
    missing, stale = reg - doc, doc - reg
    # A name that is on both sides under different arities is a wrong
    # arity, not a missing or stale name.
    wrong = {n for n, _ in missing} & {n for n, _ in stale}
    if missing:
        print("registered but not listed under that arity in the "
              "Registered Builtin Surface:")
        print(show(missing))
    if stale:
        print("listed in the Registered Builtin Surface but not registered "
              "under that arity:")
        print(show(stale))
    if wrong:
        print("wrong arity: " + " ".join(sorted(wrong)))
    if missing or stale:
        print("\nAdd each builtin to its arity list there, give it a row in the "
              "matching category table, and document it "
              "(CONTRIBUTING.md, 'Documentation').")
        return 1
    names = {n for n, _ in reg}
    print(f"builtin docs: {len(names)} registered builtins, "
          f"{len(reg)} name/arity pairs, all listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
