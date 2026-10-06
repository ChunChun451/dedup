#!/usr/bin/env python3
"""Print one SHA-256 that covers a whole directory tree or a single file (DECISIONS.md D23).

For a file: the plain SHA-256 of its bytes (same as `sha256sum`).
For a directory: SHA-256 over one line per entry, in sorted path order:
    <relative path> \\0 <type f|d|l> \\0 <permission bits, octal> \\0 <size or link target> \\0 <sha256 of file bytes> \\n
Owners and timestamps are left out, so the digest is the same on any machine that extracted the same archive.
"""
import hashlib
import os
import stat
import sys


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def tree_digest(root):
    h = hashlib.sha256()
    entries = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for name in dirnames + filenames:
            entries.append(os.path.join(dirpath, name))
    for path in sorted(entries, key=lambda p: os.path.relpath(p, root).encode()):
        rel = os.path.relpath(path, root)
        st = os.lstat(path)
        perm = oct(stat.S_IMODE(st.st_mode))
        if stat.S_ISLNK(st.st_mode):
            rec = [rel, "l", perm, os.readlink(path), ""]
        elif stat.S_ISDIR(st.st_mode):
            rec = [rel, "d", perm, "", ""]
        elif stat.S_ISREG(st.st_mode):
            rec = [rel, "f", perm, str(st.st_size), file_sha256(path)]
        else:
            sys.exit(f"unsupported file type: {path}")
        h.update(("\0".join(rec) + "\n").encode())
    return h.hexdigest()


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: treehash.py <file-or-directory>")
    p = sys.argv[1]
    print(tree_digest(p) if os.path.isdir(p) else file_sha256(p))


if __name__ == "__main__":
    main()
