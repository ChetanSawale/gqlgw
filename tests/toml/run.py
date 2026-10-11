#!/usr/bin/env python3
"""Runs the toml-test corpus (TOML 1.1.0 file list) against build/toml_decode.

Comparison rules are those of toml-test v2 (json.go): integers and strings as
text, floats numerically (any NaN equals NaN), date/times as instants.
"""
import itertools
import json
import math
import subprocess
import sys
from datetime import datetime, date, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / ".deps" / "toml-test" / "tests"
DECODER = ROOT / "build" / "toml_decode"


def norm_dt(kind, s):
    s = s.replace(" ", "T").replace("t", "T").replace("z", "Z")
    if "." in s:
        # Python handles at most microseconds: truncate (toml-test truncates to ns)
        head, frac = s.split(".", 1)
        digits = "".join(itertools.takewhile(str.isdigit, frac))
        rest = frac[len(digits):]
        s = head + "." + (digits + "000000")[:6] + rest
    if kind == "datetime":
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    if kind == "datetime-local":
        return datetime.fromisoformat(s)
    if kind == "date-local":
        return date.fromisoformat(s)
    return time.fromisoformat(s)


def equal(want, have, path):
    if isinstance(want, dict) and set(want) == {"type", "value"} and isinstance(want["value"], str):
        if not (isinstance(have, dict) and set(have) == {"type", "value"}):
            return f"{path}: expected a {want['type']} value, got {json.dumps(have)[:80]}"
        if want["type"] != have["type"]:
            return f"{path}: type {have['type']} != {want['type']}"
        w, h = want["value"], have["value"]
        t = want["type"]
        if t == "float":
            if w.lower().endswith("nan") or h.lower().endswith("nan"):
                ok = w.lower().lstrip("+-") == h.lower().lstrip("+-")
            else:
                ok = float(w) == float(h)
        elif t in ("datetime", "datetime-local", "date-local", "time-local"):
            try:
                ok = norm_dt(t, w) == norm_dt(t, h)
            except ValueError as e:
                return f"{path}: bad datetime {h!r} ({e})"
        elif t == "bool":
            ok = w.lower() == h.lower()
        else:
            ok = w == h
        return None if ok else f"{path}: {h!r} != {w!r}"
    if isinstance(want, dict):
        if not isinstance(have, dict):
            return f"{path}: expected a table"
        if set(want) != set(have):
            return f"{path}: keys {sorted(have)} != {sorted(want)}"
        for k in want:
            e = equal(want[k], have[k], f"{path}.{k}")
            if e:
                return e
        return None
    if isinstance(want, list):
        if not isinstance(have, list) or len(want) != len(have):
            return f"{path}: array length differs"
        for i, (w, h) in enumerate(zip(want, have)):
            e = equal(w, h, f"{path}[{i}]")
            if e:
                return e
        return None
    return f"{path}: unexpected expected value {want!r}"


def main():
    files = [l.strip() for l in (CORPUS / "files-toml-1.1.0").read_text().splitlines() if l.strip()]
    tests = [f for f in files if f.endswith(".toml")]
    fails = []
    nvalid = ninvalid = 0
    for f in tests:
        src = CORPUS / f
        r = subprocess.run([str(DECODER), str(src)], capture_output=True)
        if f.startswith("valid/"):
            nvalid += 1
            if r.returncode != 0:
                fails.append(f"{f}: rejected: {r.stderr.decode(errors='replace').strip()}")
                continue
            want = json.loads((CORPUS / f).with_suffix(".json").read_text())
            try:
                have = json.loads(r.stdout)
            except ValueError as e:
                fails.append(f"{f}: decoder output is not JSON: {e}")
                continue
            e = equal(want, have, "$")
            if e:
                fails.append(f"{f}: {e}")
        else:
            ninvalid += 1
            if r.returncode == 0:
                fails.append(f"{f}: accepted an invalid document")
            elif r.returncode != 1:
                fails.append(f"{f}: decoder crashed (status {r.returncode}): {r.stderr.decode(errors='replace')[:300]}")
    for line in fails:
        print("FAIL", line)
    print(f"toml-test 1.1.0: {nvalid} valid, {ninvalid} invalid, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
