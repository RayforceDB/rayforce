#!/usr/bin/env python3
"""Generate the builtin reference from the source, and fail when it differs.

The one source of a builtin's one-line summary is its entry in
src/lang/sig.c, next to its usage line (#1737).  The binary answers it from
there (.repl.signature, :h), and this script writes it into the docs:

  * the "Registered Builtin Surface" section of
    docs/docs/reference/all-functions.md: one table per arity (### Unary,
    ### Binary, ### Variadic / Special), a row per name registered with
    register_unary / register_binary / register_vary (and the *_op
    variants) in src/, with its summary;
  * the Description cell of every row naming one builtin in a table whose
    first column is "Function" (or "Form"), in all-functions.md,
    language/functions.md, storage/ipc.md, graph/vector-search.md,
    queries/*.md and namespaces/*.md.  The cell is the summary, then (but on
    the namespace and query pages) the entry's `/* docs: ... */` comment in
    sig.c, if it has one: more text for the reference rows, not compiled in.

A row that names a builtin in another sense (the Datalog `not` clause, say)
opts out with the marker <!-- docs-gen: own --> in its Description cell:
the cell is then hand-written, and invisible marker and all, left as it is.
Every opt-out must be listed in ALLOWED_OPT_OUTS below; the check prints them
all and fails on one that is not listed.
Everything else on those pages is hand-written and left as it is too.

Default mode (make docs-check, run in CI): regenerate in memory and fail when a
page differs from what is checked in, when a registered builtin has no
summary, or when the surface list and the registrations disagree in names or
arities.  --write (make docs-gen) writes the generated pages.  The output
depends only on the inputs, so running it twice changes nothing.

It reads the source text, so a registration that is compiled out on a
platform or built by a macro is counted as the text reads.

Usage: python3 scripts/check-builtin-docs.py [--write] [--page FILE]
"""
import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PAGE = ROOT / "docs/docs/reference/all-functions.md"
SIG = ROOT / "src/lang/sig.c"
# The other pages whose "Function" tables carry the summaries (no docs comment).
# The other pages whose "Function" tables carry the summaries, and whether
# their cells take the docs comment too.
OTHER_PAGES = [("docs/docs/language/functions.md", True),
               ("docs/docs/storage/ipc.md", True),
               ("docs/docs/graph/vector-search.md", True),
               ("docs/docs/namespaces/*.md", False),
               ("docs/docs/queries/*.md", False)]
# A "Function" table's first header; select.md's quick reference says "Form".
FIRST_HEADERS = ("Function", "Form")
OPT_OUT = "<!-- docs-gen: own -->"
# Every row allowed to opt out, as (page, builtin name).  An opt-out not
# listed here fails the check, so hand-written descriptions cannot grow back
# unnoticed: each is a name used in another sense, or a builtin's role on a
# topic page.
ALLOWED_OPT_OUTS = {
    # the Datalog negation clause, not the builtin `not`
    ("docs/docs/language/functions.md", "not"),
    # distinct's use on a table, in the Table Set Operations section
    ("docs/docs/language/functions.md", "distinct"),
    # pivot's aggregations: what each gives in a pivot cell
    *(("docs/docs/queries/pivot.md", n) for n in
      ("sum", "avg", "count", "min", "max", "first", "last", "med", "dev")),
}
ENTRY = re.compile(
    r'\{"((?:[^"\\]|\\.)+)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\},'
    r'(?:[ \t]*\n[ \t]*/\* docs: (.*?) \*/)?')
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
    return surface_text(page.read_text())


def surface_text(text):
    lines = text.splitlines()
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
        elif arity is not None and line.startswith("| `"):
            pairs.add((split_row(line)[0][0].strip("`"), arity))
        elif arity is not None and not line.startswith("|"):
            for n in re.findall(r"`([^`]+)`", line):
                pairs.add((n, arity))
    return pairs


def unescape(s):
    return re.sub(r"\\(.)", r"\1", s)


def summaries():
    """{name: (summary, docs comment or "")} from src/lang/sig.c."""
    out = {}
    for name, _usage, summary, docs in ENTRY.findall(SIG.read_text()):
        out[unescape(name)] = (unescape(summary), docs or "")
    return out


def split_row(line):
    """The cells of a Markdown table row as (text, start, end) spans: a |
    inside a code span, or escaped as \\|, does not split."""
    cells, tick, i, start = [], 0, line.index("|") + 1, None
    start = i
    while i < len(line):
        ch = line[i]
        if ch == "\\" and line[i + 1:i + 2] == "|":
            i += 2
            continue
        if ch == "`":
            j = i
            while j < len(line) and line[j] == "`":
                j += 1
            tick = 0 if tick == j - i else (j - i if tick == 0 else tick)
            i = j
            continue
        if ch == "|" and tick == 0:
            cells.append((line[start:i].strip(), start, i))
            start = i + 1
        i += 1
    return cells


def surface_section(reg, summ):
    """The generated "Registered Builtin Surface" section."""
    out = ["## Registered Builtin Surface", "",
           "Every builtin registered in `src/` in this checkout, by arity, with its "
           "one-line summary. The categorized reference below expands the most "
           "commonly used functions. This section and the Description cells of the "
           "reference tables are generated from `src/lang/sig.c`, the one source of "
           "the summaries that `.repl.signature` and `:h` answer: "
           "`make docs-gen` writes them, and `make docs-check` (CI) fails when a page "
           "differs (see [CONTRIBUTING](https://github.com/RayforceDB/rayforce/blob/"
           "dev/CONTRIBUTING.md#documentation)).", ""]
    for arity in ARITIES:
        out += [f"### {arity}", "", "| Function | Summary |", "|---|---|"]
        for name in sorted(n for n, a in reg if a == arity):
            out.append(f"| `{name}` | {summ.get(name, ('', ''))[0]} |")
        out.append("")
    return out


def fill_tables(text, summ, with_docs, opted=None):
    """Write each builtin's summary into the Description cell of the
    "Function" tables of a page; the names of the rows that opt out go to
    `opted`."""
    lines, header = text.split("\n"), None
    for k, line in enumerate(lines):
        if not line.startswith("|"):
            header = None
            continue
        cells = split_row(line)
        if header is None:
            header = [c[0] for c in cells]
            continue
        if header[0] not in FIRST_HEADERS or "Description" not in header:
            continue
        m = re.fullmatch(r"\[?`([^`]+)`\]?(?:\([^)]*\))?", cells[0][0])
        if not m or m.group(1) not in summ:
            continue
        summary, docs = summ[m.group(1)]
        cell = summary + (docs if with_docs else "")
        text, a, b = cells[header.index("Description")]
        if OPT_OUT in text:
            if opted is not None:
                opted.append(m.group(1))
            continue
        lines[k] = line[:a] + " " + cell + " " + line[b:]
    return "\n".join(lines)


def generate(page_text, reg, summ, opted=None):
    """The all-functions page with its generated parts rewritten; the names
    of its rows that opt out go to `opted`."""
    lines = page_text.split("\n")
    start = next(i for i, l in enumerate(lines)
                 if l.startswith("## Registered Builtin Surface"))
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith("## ")), len(lines))
    lines[start:end] = surface_section(reg, summ)
    return fill_tables("\n".join(lines), summ, True, opted)


def unlisted(opt_outs):
    """The opt-outs, as (page, name), that ALLOWED_OPT_OUTS does not list."""
    return sorted(set(opt_outs) - ALLOWED_OPT_OUTS)


def self_test(reg, summ):
    """The marker is collected on every kind of generated page, the reference
    page included, and an opt-out that is not listed is caught.  A failure
    here is the script's own bug: it would let an opt-out pass unchecked."""
    table = ("| Function | Type | Flags | Description | Example |\n"
             "|---|---|---|---|---|\n"
             f"| `count` | unary | — | hand-written {OPT_OUT} | e |\n"
             "| `sum` | unary | — | old | e |\n")
    page = "## Registered Builtin Surface\n\n## Next\n\n" + table
    ref, other = [], []
    out = generate(page, reg, summ, ref)
    fill_tables(table, summ, False, other)
    problems = []
    if ref != ["count"]:
        problems.append(f"reference page opt-outs collected as {ref}")
    if other != ["count"]:
        problems.append(f"other page opt-outs collected as {other}")
    if f"hand-written {OPT_OUT}" not in out or f"| {summ['sum'][0]}" not in out:
        problems.append("an opted-out cell was rewritten, or a generated one was not")
    if not unlisted([("docs/docs/reference/all-functions.md", "count")]):
        problems.append("an unlisted opt-out on the reference page is not caught")
    return problems


def show(pairs):
    return "  " + " ".join(f"{n} ({a})" for n, a in sorted(pairs))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--page", type=Path, default=DEFAULT_PAGE)
    parser.add_argument("--write", action="store_true",
                        help="write the generated pages instead of checking them")
    args = parser.parse_args()
    reg, summ = registered(), summaries()
    bad = 0
    nosumm = sorted({n for n, _ in reg if not summ.get(n, ("", ""))[0]})
    if nosumm:
        print("registered but without a summary in src/lang/sig.c:")
        print("  " + " ".join(nosumm))
        bad = 1
    piped = sorted(n for n, (t, _) in summ.items() if "|" in t)
    if piped:
        print("a summary may not hold a |: " + " ".join(piped))
        bad = 1
    # one style: a summary ends without a full stop (an ellipsis is text)
    dotted = sorted(n for n, (t, _) in summ.items()
                    if t.endswith(".") and not t.endswith("..."))
    if dotted:
        print("a summary ends without a full stop: " + " ".join(dotted))
        bad = 1
    problems = self_test(reg, summ)
    if problems:
        print("self-test of the generator failed:")
        for p in problems:
            print("  " + p)
        bad = 1
    opt_outs = []
    ref_opted = []
    pages = {args.page: generate(args.page.read_text(), reg, summ, ref_opted)}
    for name in ref_opted:
        opt_outs.append((str(args.page.resolve().relative_to(ROOT)), name))
    for pattern, with_docs in OTHER_PAGES:
        for path in sorted(ROOT.glob(pattern)):
            opted = []
            pages[path] = fill_tables(path.read_text(), summ, with_docs, opted)
            rel = str(path.relative_to(ROOT))
            for name in opted:
                opt_outs.append((rel, name))
    stale_pages = [p for p, t in pages.items() if p.read_text() != t]
    print(f"rows opted out of the generated descriptions ({len(opt_outs)}):")
    for page, name in sorted(opt_outs):
        print(f"  {page}: {name}")
    stray = unlisted(opt_outs)
    if stray:
        print("opted out but not in ALLOWED_OPT_OUTS (scripts/check-builtin-docs.py):")
        for page, name in stray:
            print(f"  {page}: {name}")
        bad = 1
    if args.write:
        for p in stale_pages:
            p.write_text(pages[p])
            print(f"wrote {p.relative_to(ROOT)}")
    elif stale_pages:
        for p in stale_pages:
            old, new = p.read_text().splitlines(), pages[p].splitlines()
            diff = list(difflib.unified_diff(old, new, str(p.relative_to(ROOT)),
                                             "generated", lineterm="", n=0))
            print("\n".join(diff[:40]))
            if len(diff) > 40:
                print(f"... {len(diff) - 40} more lines")
        print("\nThe pages above differ from what src/lang/sig.c generates: "
              "run `make docs-gen` and commit the result.")
        bad = 1
    doc = surface(args.page) if not args.write else surface_text(pages[args.page])
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
        print("\nGive each builtin an entry in src/lang/sig.c, run `make docs-gen`, "
              "give it a row in the matching category table, and document it "
              "(CONTRIBUTING.md, 'Documentation').")
        bad = 1
    if bad:
        return 1
    names = {n for n, _ in reg}
    print(f"builtin docs: {len(names)} registered builtins, "
          f"{len(reg)} name/arity pairs, all listed with a summary"
          + (", pages written" if stale_pages and args.write else ", pages current"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
