#!/usr/bin/env python3
"""Generate a C++ header from .sql files in a directory.

For each .sql file:
  1. Strip SQL comments (-- lines)
  2. Split on semicolons to get individual statements
  3. Emit a std::vector<std::string> constant with the statements

Usage:
    python3 embed_sql.py <sql_dir> <output_file>
"""

import os
import re
import sys
from pathlib import Path


def strip_comments(sql: str) -> str:
    """Remove SQL line comments (-- to end of line)."""
    return re.sub(r"--[^\n]*", "", sql)


def split_statements(sql: str) -> list[str]:
    """Split SQL on semicolons, strip whitespace, drop empties."""
    stmts = []
    for raw in sql.split(";"):
        # Collapse runs of whitespace
        s = re.sub(r"[ \t]+", " ", raw)
        s = re.sub(r"\n ", "\n", s)
        s = re.sub(r"\n+", "\n", s)
        s = s.strip()
        if s:
            stmts.append(s)
    return stmts


def escape_cpp(s: str) -> str:
    """Escape a string for inclusion in a C++ string literal."""
    s = s.replace("\\", "\\\\")
    s = s.replace('"', '\\"')
    s = s.replace("\n", "\\n")
    return s


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <sql_dir> <output_file>", file=sys.stderr)
        sys.exit(1)

    sql_dir = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    sql_files = sorted(sql_dir.glob("*.sql"))
    if not sql_files:
        print(f"No .sql files found in {sql_dir}", file=sys.stderr)
        sys.exit(1)

    lines = [
        "#pragma once",
        "// Auto-generated from sql/*.sql — do not edit.",
        "// Regenerated at build time by cmake/embed_sql.py.",
        "",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace http_client {",
        "namespace sql {",
        "",
    ]

    for sql_file in sql_files:
        name = sql_file.stem  # http_verbs.sql -> http_verbs
        raw = sql_file.read_text()
        cleaned = strip_comments(raw)
        stmts = split_statements(cleaned)

        lines.append(f"inline const std::vector<std::string> {name} = {{")
        for i, stmt in enumerate(stmts):
            comma = "," if i < len(stmts) - 1 else ""
            lines.append(f'    "{escape_cpp(stmt)}"{comma}')
        lines.append("};")
        lines.append("")

    lines.append("} // namespace sql")
    lines.append("} // namespace http_client")
    lines.append("")

    output_file.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
