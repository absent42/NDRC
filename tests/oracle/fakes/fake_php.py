# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Fake php for test_reference.py, standing in for `php drb.php ...`.

reference.py invokes it as [php, drb-script, target, ..., g.json, g.DDB],
so argv[1] is the drb.php path - ignored here, the same way the real php
binary would just treat it as the script to run. Mode is carried through
the FAKE_PHP_MODE environment variable, for the same reason fake_drf.py
uses one rather than argv - see that file's docstring.

Modes:
  ok (default)  writes STDOUT_MARKER then a fixed g.DDB, exits 0
  fail          exits 2 without writing anything
  noddb         exits 0 but writes no g.DDB (the "DRB produced no DDB" path)
  sleep         sleeps past the caller's timeout, then behaves as ok
  xmb           behaves as ok, and additionally writes 0.XMB (FIXED_XMB),
                for run_reference_from_json's xmb_files collection

STDOUT_MARKER is distinct from fake_drf's own marker so a test can assert
that reference.py keeps DRF's and DRB's stdout apart where it says it
does (FromJsonRefResult.drb_stdout).
"""
import os
import sys
import time

FIXED_DDB = bytes(range(16)) * 4
FIXED_XMB = bytes(range(200, 216))
STDOUT_MARKER = "fake_php: stdout marker\n"


def main() -> int:
    mode = os.environ.get("FAKE_PHP_MODE", "ok")
    if mode == "fail":
        sys.stderr.write("fake_php: forced failure\n")
        return 2
    if mode == "sleep":
        time.sleep(5)
    if mode == "noddb":
        return 0
    sys.stdout.write(STDOUT_MARKER)
    with open("g.DDB", "wb") as f:
        f.write(FIXED_DDB)
    if mode == "xmb":
        with open("0.XMB", "wb") as f:
            f.write(FIXED_XMB)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
