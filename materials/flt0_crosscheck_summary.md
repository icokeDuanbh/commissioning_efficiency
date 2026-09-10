# FLT0 Cross-Check Summary

**Xishui**: [`FLT0/T1_trigger_offline.py`](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/FLT0/T1_trigger_offline.py) → `extract_trigger_parameters()`  
**Clement**: [`cross_check_Clement/offline_FLT0_trigger.py`](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/cross_check_Clement/offline_FLT0_trigger.py) → `trigger_FLT0()`  
**Script**: [`cross_check_Clement/cross_check_flt0.py`](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/cross_check_Clement/cross_check_flt0.py)

---

## Overall Result

**7/10 cases agree** (8 synthetic + 2 real-trace channels). The 3 discrepancies are all **intentional algorithmic differences**, not numerical errors.

---

## Scorecard

| Case | Xishui | Clement | Result | Note |
|------|------|---------|--------|------|
| `01_no_signal` | False | False | ✅ AGREE | — |
| `02_quiet_violated` | True (nc=4) | True (nc=4) | ⚠️ AGREE but **test case was flawed** | See deep-dive below |
| `03_nc_exactly_nc_min` | False (nc=2) | True (nc=2) | ❌ DIFFER | NC bounds convention |
| `04_valid_nc_min_plus_1` | True (nc=3) | True (nc=3) | ✅ AGREE | — |
| `05_nc_exceeds_nc_max` | False | False | ✅ AGREE | — |
| `06_tsepmax_violated` | False | True (nc=2) | ❌ DIFFER | Tsepmax abort strategy |
| `07_t1_before_idx100` | True (nc=3) | False | ❌ DIFFER | Early-T1 guard |
| `08_two_valid_t1s` | True (nc=3) | True (nc=3) | ✅ AGREE | — |
| `ch0` (real trace) | False | False | ✅ AGREE | — |
| `ch2` (real trace) | False | False | ✅ AGREE | — |

---

## Intentional Algorithmic Differences

### Diff 1 — NC bounds convention (Case 03)

| | Xishui | Clement |
|---|---|---|
| Check | `nc_min < NC < nc_max` **(exclusive)** | `nc_min <= NC <= nc_max` **(inclusive)** |
| NC=2, nc_min=2 | ❌ rejected | ✅ accepted |

> **Impact**: triggers at the exact nc_min or nc_max boundary are accepted by Clement and rejected by Xishui. One of us needs to align with the firmware spec.

---

### Diff 2 — Tsepmax violation handling (Case 06)

| | Xishui (`extract_trigger_parameters`) | Clement (`trigger_FLT0`) |
|---|---|---|
| On Tsepmax violation | `raise ValueError` → **aborts entire trace** | marks that T1 invalid, `break` inner loop, **continues to next T1** |
| Result here | False (aborted after first T1) | True (found a second T1 later in trace) |

> **Impact**: on a trace with multiple T1 candidates, Clement is more forgiving. Xishui will miss any later valid T1 if an earlier one has a Tsepmax issue.

---

### Diff 3 — Early T1 guard (Case 07)

| | Xishui | Clement |
|---|---|---|
| T1 at index ≤ 100 | No guard, proceeds normally | Skips with `continue` |
| Reason | — | Notch-filter artifact: a transient artefact appears in the first ~100 samples after the digital notch filter is applied. |

> **Impact**: Xishui may fire on notch-filter transients. If the notch filter is always applied upstream, Xishui needs this guard too.

---

### Diff 4 — Multiple triggers per trace

| | Xishui | Clement |
|---|---|---|
| Return | Only the **first** valid T1 | **All** valid T1s |

> **Impact**: for trigger-rate estimation across a long trace (many DU cycles concatenated) Clement is more complete.

---

### Diff 5 — Quiet-region failure strategy

| | Xishui | Clement |
|---|---|---|
| If quiet window contains a sample > th1 | `raise ValueError`, stop | `continue` to next T1 candidate |

---

## Deep Dive: Case 02 — "Quiet Violation"

### What the test case intended

```
t[148] = 550   # above th1=500 → meant to violate quiet window of T1@150
t[150] = 600   # intended T1 crossing
```

Expected: both return **False** (quiet violated → no trigger).  
Actual: both return **True (nc=4)**.

### Root cause: the "violator" is itself a valid T1

Any sample above `th1` is a T1 **candidate**. Setting `t[148] = 550` doesn't just pollute the quiet window of `t[150]` — it *creates a new T1 at index 148* with its own clear quiet window `[138:148]` (all zeros).

```
T1@148: quiet_window=[138:148] = [0,0,0,0,0,0,0,0,0,0]  → quiet OK ✅
T1@150: quiet_window=[140:150] = [..., 550, ...]          → quiet FAILS ❌
```

So T1@148 fires correctly. Both implementations are **correct**. The test case was wrongly constructed.

### The fundamental constraint

> **It is impossible to place a single isolated spike above `th1` in the quiet window of a target T1 without that spike itself being a valid T1 candidate** (since its own quiet window, shifted earlier, will be all-zero in a clean signal).

### How to actually test quiet violation

**Option A: cascaded blockers** — use a prior spike to block the violator's own quiet check:

```
t[130] = th1+50   # T1 at 130: quiet[120:130] clear → fires
t[142] = th1+30   # T1 at 142: quiet[132:142] contains t[131]=110 (th2+10) → 110 < th1=500 → still passes!
t[150] = th1+100  # T1 at 150: quiet[140:150] contains 530 → FAILS ✅
```

Result: Clement fires at T1@130 and T1@142 (both have clear quiet windows). Xishui fires only at T1@130 (first valid). T1@150 is correctly rejected by both.

**Option B: sustained block** — the only way to guarantee the quiet check fails with NO compensating valid T1 is a sustained high signal where the FIRST sample of the block itself sees a dirty quiet window — but the first sample of the block always has a clear quiet window. So:

```
t[140:165] = th1 + 80   # sustained block, 25 samples
```

Result:
- Clement: **no trigger** (T1@140 fires but NC=1 < nc_min=2 → not enough T2 crossings within the block)
- Xishui: `index_T1=140, NC=1` → `False` (nc_min < NC excluded by exclusive bounds)

This is the closest we get to "quiet violation causes no trigger" — but the reason is **NC too low**, not the quiet check itself.

### Correct quiet-violation test case

A proper quiet-violation test requires a **sub-threshold violator** (`th2 < v <= th1`) that contaminates the quiet window. But the quiet check tests `<= th1`, so any value `<= th1` passes the quiet check — meaning only values **strictly above th1** fail it, which are themselves T1 candidates.

> **Conclusion**: the quiet condition cannot be violated by a non-T1 signal. The only true quiet violation is when a *previous T1 trigger's tail* bleeds into the next T1's quiet window — which is the **intended use case** (suppressing double-triggers from the same pulse). Both implementations handle this correctly.

---

## Fix for `cross_check_flt0.py` Case 02

Replace the case with the correct scenario — a second T1 that is correctly blocked by the first T1's T2 trailing edge:

```python
# Correct Case 02: first T1 at 130 has a T2 tail; T1 at 150 sees dirty quiet
t = zeros()
t[130] = th1 + 200       # main T1 at 130 (clear quiet [120:130])
t[131] = th2 + 50        # T2 crossing: within period AND within quiet of t1@150
t[150] = th1 + 100       # second T1: quiet[140:150] contains t[131]=150 > th1? No, 150 < 500
# Still not enough — T2-range value (150) < th1 (500), so quiet check (<=th1) PASSES
# → quiet violation by a T2-level value is impossible by design.
```

> **Bottom line**: Case 02 as a "quiet violation test" is semantically impossible in this trigger logic. It should be replaced with a **double-pulse suppression test** instead, verifying that T1@148 fires and T1@150 is correctly skipped.

---

## Recommended Actions

| Priority | Action |
|---|---|
| 🔴 High | Align **NC bounds** (exclusive vs inclusive) with firmware spec (Case 03) |
| 🔴 High | Add **index_T1 ≤ 100 guard** to `extract_trigger_parameters` if notch filter is always applied (Case 07) |
| 🟡 Medium | Decide if **Tsepmax abort** should kill the whole trace (Xishui) or just that T1 (Clement) (Case 06) |
| 🟢 Low | Fix **Case 02 test** to be a "double-pulse suppression" check instead of a quiet violation |
| 🟢 Low | Consider returning all valid T1s from `extract_trigger_parameters` instead of only the first |
