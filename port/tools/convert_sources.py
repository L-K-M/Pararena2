#!/usr/bin/env python3
"""Convert the original THINK C sources for a modern compiler, into a build
directory (the files in Sources/ and Headers/ are never modified):

  1. classic Mac CR line endings -> LF
  2. Pascal string literals "\pfoo" -> "\003foo" (length-prefixed with an
     exact 3-digit octal escape, so a following digit can't extend it)

Usage: convert_sources.py <repo_root> <out_dir> [file.c ...]
With no file list, converts all Headers/*.h and the given Sources files.
"""
import os
import re
import sys

PSTR_RE = re.compile(rb'"\\p((?:[^"\\]|\\.)*)"')

ESCAPES = {
    b"n": 1, b"t": 1, b"r": 1, b"0": 1, b"\\": 1, b'"': 1, b"'": 1,
}


def pstr_len(body: bytes) -> int:
    """Count characters in a C string literal body (escapes count as one)."""
    n = 0
    i = 0
    while i < len(body):
        if body[i:i+1] == b"\\" and i + 1 < len(body):
            i += 2
        else:
            i += 1
        n += 1
    return n


def convert(data: bytes) -> bytes:
    data = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")

    def repl(m):
        body = m.group(1)
        return b'"\\%03o' % pstr_len(body) + body + b'"'

    return PSTR_RE.sub(repl, data)


def main():
    root, out = sys.argv[1], sys.argv[2]
    files = sys.argv[3:]
    os.makedirs(out, exist_ok=True)

    todo = []
    hdr_dir = os.path.join(root, "Headers")
    for h in sorted(os.listdir(hdr_dir)):
        if h.endswith(".h"):
            todo.append(os.path.join(hdr_dir, h))
    for f in files:
        todo.append(os.path.join(root, "Sources", f))

    for path in todo:
        with open(path, "rb") as fh:
            data = convert(fh.read())
        dst = os.path.join(out, os.path.basename(path))
        # only rewrite when changed to keep build incremental
        if not (os.path.exists(dst) and open(dst, "rb").read() == data):
            with open(dst, "wb") as fh:
                fh.write(data)
    print(f"converted {len(todo)} files -> {out}")


if __name__ == "__main__":
    main()
