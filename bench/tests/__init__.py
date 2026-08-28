# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Tests for the benchmark harness.

Run from the repository root:

    C:/Python312/python.exe -m unittest discover -s bench/tests -t .

`bench/` is a directory of scripts, not an installed package — `run_matrix.py` and `metrics.py`
are imported by path, not by distribution. So each test module puts `bench/` on `sys.path`
itself rather than relying on this file having been imported, which keeps the modules runnable
under `python -m unittest bench.tests.test_metrics`, under discovery with a different top-level
directory, and directly. The insertion below is the convenience path for the common case; it is
deliberately duplicated in the test modules rather than depended on.
"""

import os
import sys

_BENCH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)
