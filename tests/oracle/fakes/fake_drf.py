# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Fake drf.exe for test_reference.py.

reference.py always invokes drf with fixed filenames in the run directory
(g.DSF in, g.json out), so this reads no argv - mode is carried entirely
through the FAKE_DRF_MODE environment variable, which subprocess.run
inherits from the test process. A marker file would not work here: the
fake has no way to see test-controlled state that argv and the
environment do not carry.

Modes:
  ok (default)  writes STDOUT_MARKER then a fixed g.json, exits 0
  fail          exits 2 without writing anything
  sleep         sleeps past the caller's timeout, then behaves as ok

STDOUT_MARKER is distinct from fake_php's own marker so a test can assert
that reference.py keeps DRF's and DRB's stdout apart where it says it
does (FromJsonRefResult.drb_stdout).
"""
import os
import sys
import time

FIXED_JSON = '{"fake": "drf", "YEARHIGH": 20, "YEARLOW": 26}'
STDOUT_MARKER = "fake_drf: stdout marker\n"


def main() -> int:
    mode = os.environ.get("FAKE_DRF_MODE", "ok")
    if mode == "fail":
        sys.stderr.write("fake_drf: forced failure\n")
        return 2
    if mode == "sleep":
        time.sleep(5)
    sys.stdout.write(STDOUT_MARKER)
    with open("g.json", "w", encoding="latin-1") as f:
        f.write(FIXED_JSON)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
