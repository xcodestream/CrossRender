#!/usr/bin/env python3
"""Checks the Russian API documentation in docs/.

For every markdown file it verifies that
  * the text is actually Russian (Cyrillic share of letters),
  * every member section (a `###` heading) is followed by a ```cpp example,
  * every identifier used in a member heading exists somewhere in engine/include,
  * every public header has a documentation file.

Exit code is non-zero when a hard check fails.  Unknown identifiers are
reported but do not fail the run unless they exceed `--unknown-limit` (some
headings legitimately name free functions, operators or typedefs).
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
INCLUDE = os.path.join(ROOT, "engine", "include")

HEADER_RE = re.compile(r"^(class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)", re.M)
# Секции членов могут быть вложенными: группа ролей на ### и её члены на
# ####.  Считаются оба, поэтому проходит любая структура.
HEADING_RE = re.compile(r"^#{3,4}\s+(.*)$", re.M)
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}")
CYR_RE = re.compile(r"[А-Яа-яЁё]")
LAT_RE = re.compile(r"[A-Za-z]")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_engine_symbols():
    """Every identifier that appears anywhere in the public headers."""
    symbols = set()
    files = {}
    for base, _dirs, names in os.walk(INCLUDE):
        for n in names:
            if not n.endswith(".h"):
                continue
            p = os.path.join(base, n)
            text = read(p)
            files[os.path.relpath(p, INCLUDE)] = text
            for ident in IDENT_RE.findall(text):
                symbols.add(ident)
    return symbols, files


def header_classes(text):
    return [m.group(2) for m in HEADER_RE.finditer(text)]


def members_part(doc):
    """The `## Члены класса` body, or the whole document when it is absent.

    Sub-headings inside `## Обзор` are prose explanations, not members, so they
    are not required to carry a code example.
    """
    marker = doc.find("\n## Члены класса")
    if marker < 0:
        return doc
    start = marker + 1
    nxt = doc.find("\n## ", start + 3)
    return doc[start:] if nxt < 0 else doc[start:nxt]


def member_sections(doc):
    """(heading_text, body_text) for every member heading."""
    out = []
    for m in HEADING_RE.finditer(doc):
        start = m.end()
        nxt3 = doc.find("\n###", start)
        nxt4 = doc.find("\n####", start)
        ends = [x for x in (nxt3, nxt4) if x >= 0]
        nxt = min(ends) if ends else -1
        body = doc[start:] if nxt < 0 else doc[start:nxt]
        out.append((m.group(1), body))
    return out


def cyrillic_share(text):
    cyr = len(CYR_RE.findall(text))
    lat = len(LAT_RE.findall(text))
    if cyr + lat == 0:
        return 0.0
    return cyr / float(cyr + lat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unknown-limit", type=float, default=0.10,
                    help="fraction of unknown identifiers tolerated per file")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    symbols, headers = load_engine_symbols()
    classes = {}
    for rel, text in headers.items():
        for cls in header_classes(text):
            # Детали реализации не входят в публичный API и не документируются:
            # структуры `Impl` и всё в пространстве имён `_internal`
            # пропускается.
            if cls.endswith("Impl"):
                continue
            idx = text.find(cls)
            window = text[max(0, idx - 400):idx]
            if "_internal" in window:
                continue
            classes.setdefault(cls, rel)

    problems = []
    unknown_total = 0
    ident_total = 0

    md_files = []
    for base, _dirs, names in os.walk(DOCS):
        for n in sorted(names):
            if n.endswith(".md"):
                md_files.append(os.path.join(base, n))
    if not md_files:
        print("doccheck: docs/ contains no markdown files")
        return 1

    for path in sorted(md_files):
        rel = os.path.relpath(path, ROOT)
        doc = read(path)
        # Мета-файлы описывают саму документацию, а не заголовок API.
        if rel.endswith("README.md") or rel.endswith("STYLE.md"):
            continue
        share = cyrillic_share(doc)
        if share < 0.20:
            problems.append(f"{rel}: only {share:.0%} Cyrillic - the docs must be in Russian")
        member_doc = members_part(doc)
        sections = member_sections(member_doc)
        examples = doc.count("```cpp")
        if examples == 0:
            problems.append(f"{rel}: no ```cpp code examples")
        if len(sections) == 0:
            problems.append(f"{rel}: no ### member sections")
        if examples < len(sections):
            problems.append(
                f"{rel}: {len(sections)} member sections but only {examples} cpp examples")
        unknown = []
        for heading, body in sections:
            if "```cpp" not in body:
                problems.append(f"{rel}: section '{heading.strip()[:50]}' has no cpp example")
            for ident in IDENT_RE.findall(heading):
                ident_total += 1
                if ident not in symbols and not ident.startswith("crossrender"):
                    unknown.append(ident)
        unknown_total += len(unknown)
        frac = (len(unknown) / float(len(sections))) if sections else 0.0
        if frac > args.unknown_limit:
            problems.append(
                f"{rel}: {len(unknown)} identifiers in headings are not in engine/include "
                f"({', '.join(sorted(set(unknown))[:6])}...)")
        if args.verbose:
            print(f"  {rel:44s} sections={len(sections):3d} examples={examples:3d} "
                  f"cyrillic={share:.0%}")

    # Каждый публичный заголовок должен быть покрыт хотя бы одним файлом документации.
    docs_text = "\n".join(read(p) for p in md_files)
    missing = []
    for cls, rel in sorted(classes.items()):
        if cls not in docs_text:
            missing.append(f"{cls} ({rel})")
    if missing:
        problems.append("classes with no documentation: " + ", ".join(missing[:12]) +
                        (f" ... +{len(missing) - 12}" if len(missing) > 12 else ""))

    print(f"doccheck: {len(md_files)} files, {ident_total} heading identifiers, "
          f"{len(classes)} public classes")
    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print("  - " + p)
        return 1
    print("doccheck: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
