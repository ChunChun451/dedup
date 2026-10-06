#!/usr/bin/env python3
"""Read JSON from stdin and check that every dotted key path given as an argument exists (e.g. stages.chunk.busy_s).

Usage: some_command --json | python3 tests/assert_keys.py a.b c
Exit 0 if all keys exist; otherwise print the missing ones and exit 1.
"""
import json
import sys


def has(obj, path):
    for part in path.split("."):
        if isinstance(obj, dict) and part in obj:
            obj = obj[part]
        else:
            return False
    return True


def main():
    data = json.load(sys.stdin)
    missing = [k for k in sys.argv[1:] if not has(data, k)]
    for k in missing:
        print("missing key:", k)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
