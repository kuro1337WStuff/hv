"""Offline CLI fixtures. Run: python -m unittest discover -s lab/tests -p test_journal_gate.py"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().with_name("journal_gate.py")


class JournalGateTest(unittest.TestCase):
    def test_event_against_published_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*args, stdin=None):
                return subprocess.check_output(["git", *args], cwd=root, input=stdin, text=True).strip()

            git("init", "-q")
            blob = git("hash-object", "-w", "--stdin", stdin="fixture\n")
            for name, mode in (("041.md", "100644"), ("042.md", "100644"), ("043.md", "120000")):
                git("update-index", "--add", "--cacheinfo", f"{mode},{blob},journal/trials/{name}")
            base = git("write-tree")
            # A new record present in the hypothetical PR tree is not on main.
            git("update-index", "--add", "--cacheinfo", f"100644,{blob},journal/trials/044.md")
            git("write-tree")

            first = "Journal: journal/trials/041.md"
            for body, accepted in (
                (first, True),
                ("Change summary\r\n\r\n" + first + "\r\nJournal: journal/trials/042.md\r\n", True),
                (None, False),
                ("See journal/trials/041.md", False),
                (" " + first, False),
                (first + " # extra", False),
                (first + "\nJournal: journal/trials/044.md", False),
                ("Journal: journal/trials/043.md", False),
                ("Journal: journal/trials/$(whoami).md", False),
            ):
                with self.subTest(body=body):
                    event = root / "event.json"
                    event.write_text(json.dumps({"pull_request": {"body": body}}), encoding="utf-8")
                    result = subprocess.run(
                        [sys.executable, "-I", str(CHECKER), base, str(event)],
                        cwd=root, text=True, capture_output=True,
                    )
                    self.assertEqual(result.returncode, 0 if accepted else 1, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
