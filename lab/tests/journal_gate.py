"""Validate Journal trailers against the named Git tree, never the working tree."""

import json
from pathlib import Path
import re
import subprocess
import sys


TRAILER = re.compile(r"Journal: (journal/trials/[0-9]{3}\.md)")


def check(base, body):
    paths = set()
    for line in (body or "").splitlines():
        if line.startswith("Journal:"):
            match = TRAILER.fullmatch(line)
            if not match:
                raise ValueError("Use an exact line: Journal: journal/trials/NNN.md")
            paths.add(match[1])
    if not paths:
        raise ValueError("Runtime changes require a line: Journal: journal/trials/NNN.md")

    entries = subprocess.check_output(
        ["git", "ls-tree", "-z", "--full-tree", base, "--", *sorted(paths)]
    )
    for entry in entries.split(b"\0"):
        if entry:
            metadata, name = entry.split(b"\t", 1)
            if metadata.split()[:2] in ([b"100644", b"blob"], [b"100755", b"blob"]):
                paths.remove(name.decode("ascii"))
    if paths:
        raise ValueError("Not published as a regular file on base main: " + ", ".join(sorted(paths)))


if __name__ == "__main__":
    base, event_path = sys.argv[1:]
    body = json.loads(Path(event_path).read_text(encoding="utf-8"))["pull_request"]["body"]
    try:
        check(base, body)
    except ValueError as exc:
        sys.exit(str(exc))
    print("All cited trial records are published on base main.")
