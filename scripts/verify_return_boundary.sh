#!/usr/bin/env bash
# Reproducible verification of the routing-snapshot publish boundary work.
#
# Covers the conference return (mix-minus) defect investigation end to end:
#   1. unit suite (feedback-loop rejection, mix-minus, frozen snapshots)
#   2. matrix/concurrency boundary regression (skipped revisions, per-block
#      single-generation pinning, mailbox newest-revision semantics,
#      exact-direction edge queries, mute + dropout soak)
#   3. end-to-end demo and its return-bus frequency content: each Send bus
#      must read ~0.0000 magnitude at its own remote's signature frequency
#
# Exits non-zero on any failure, so the fix conclusion is re-runnable. All
# build outputs stay under build/ (git-ignored); narrative evidence is
# written to evidence/ and committed.
set -euo pipefail
cd "$(dirname "$0")/.."

EVID=evidence
mkdir -p "$EVID" out

echo "== [1/3] build =="
make build/mixtests build/regress_conc build/mixdemo

echo "== [2/3] unit + concurrency/matrix boundary regression =="
./build/mixtests | tee "$EVID/unit_tests.log"
./build/regress_conc | tee "$EVID/matrix_competition_regression.log"

echo "== [3/3] end-to-end demo: return-bus frequency content =="
./build/mixdemo > "$EVID/demo_return_frequencies.log" 2>&1
cat "$EVID/demo_return_frequencies.log"

# Hard evidence: the two Send buses (rows bus 2 / bus 3 in the demo) must
# show zero magnitude at their own remote's signature bin. The demo already
# exits non-zero on a matrix mismatch; this additionally pins the numeric
# self-exclusion so a silent loosening of the threshold cannot pass.
python3 -I - "$EVID/demo_return_frequencies.log" <<'PY'
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
rows = re.findall(r"^bus \d+ magnitudes: (.*)$", text, re.MULTILINE)
assert len(rows) >= 4, f"expected >=4 magnitude rows, got {len(rows)}"
mags = [[float(x) for x in r.split()] for r in rows]
# sendA is bus 2: own remote remA is source index 1 -> that bin must be 0.
# sendB is bus 3: own remote remB is source index 2 -> that bin must be 0.
checks = [(2, 1), (3, 2)]
ok = True
for bus, src in checks:
    v = mags[bus][src]
    status = "ok" if v == 0.0 else "FAIL"
    print(f"mix-minus self-exclusion: bus {bus} src {src} magnitude={v:.4f} [{status}]")
    ok = ok and v == 0.0
if "matrix: PASS" not in text:
    print("matrix self-check did not PASS")
    ok = False
sys.exit(0 if ok else 1)
PY

echo
echo "ALL VERIFICATION PASSED"
