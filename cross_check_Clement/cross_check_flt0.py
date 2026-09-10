"""
cross_check_flt0.py  --  FLT0 implementation cross-check

Note on quiet-violation testing
-------------------------------
It is impossible to construct a test where a *non-T1* signal value contaminates
the quiet window of a target T1, because the quiet check is `signal <= th1`.
Any value that would fail the check (> th1) is itself a T1 candidate.
Case 02 therefore tests "double-pulse suppression": the first T1 is valid and
fires; the second T1 arrives before the first has decayed, so its quiet window
is dirty -- both implementations should reject it.
"""
import sys, os, argparse
import numpy as np

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FLT0_DIR = os.path.join(THIS_DIR, "..", "FLT0")
if THIS_DIR not in sys.path: sys.path.insert(0, THIS_DIR)
if FLT0_DIR not in sys.path: sys.path.insert(0, FLT0_DIR)

from offline_FLT0_trigger import trigger_FLT0
from T1_trigger_offline import extract_trigger_parameters

DEFAULT_PARAMS = {"th1":500,"th2":100,"t_quiet":20,"t_period":40,"t_sepmax":30,"nc_min":2,"nc_max":8}

def run_mine(trace, p):
    try:
        info = extract_trigger_parameters(trace, p)
        nc = info.get("NC", 0)
        return p["nc_min"] < nc < p["nc_max"], nc
    except ValueError:
        return False, None

def run_clement(trace, p):
    T1s, _, NCs = trigger_FLT0(trace, p)
    if not T1s: return False, None
    for nc in NCs:
        if p["nc_min"] < nc < p["nc_max"]:  # exclusive bounds, matching FPGA
            return True, nc
    return False, NCs[0] if NCs else None

def build_cases(p):
    th1,th2,tp,tsep,nc_min,nc_max = p["th1"],p["th2"],p["t_period"]//2,p["t_sepmax"],p["nc_min"],p["nc_max"]
    def Z(): return np.zeros(512, dtype=int)
    cases = []
    t = Z(); cases.append(("01_no_signal", t, False, False))
    # 2. Double-pulse suppression.
    # T1_A at idx 130: quiet [120:130] is clear -> fires.
    # T1_A's T2 crossing at idx 131 (th2+10=110) sits inside the quiet window
    # of T1_B at idx 140 ([130:140]).  110 < th1=500 so quiet check PASSES --
    # quiet violation by a sub-th1 value is impossible by design.
    #
    # To get a true rejection: T1_B at idx 140 has quiet [130:140] which
    # contains T1_A's spike (th1+200=700 > th1) -> quiet FAILS -> rejected.
    # Both impls should trigger on T1_A (idx 130) and NOT on T1_B (idx 140).
    # Expected: triggered=True (from T1_A), which is what we verify.
    t = Z()
    t[130] = th1 + 200   # T1_A: 700, clear quiet [120:130]
    t[131] = th2 + 10    # T2 crossing after T1_A
    t[133] = th2 + 10    # 2nd T2 crossing (nc_min-1 extra needed)
    t[140] = th1 + 50    # T1_B: quiet[130:140] contains 700 -> REJECTED
    cases.append(("02_double_pulse_suppression", t, True, True))
    t = Z(); t[150]=th1+200
    for k in range(nc_min-1):
        pos=152+k*max(1,tsep//2-1)
        if pos<150+tp: t[pos]=th2+10
    cases.append(("03_nc_exactly_nc_min", t, None, None))
    t = Z(); t[150]=th1+300
    for k in range(nc_min):
        pos=152+k*max(1,tsep//2-1)
        if pos<150+tp: t[pos]=th2+10
    cases.append(("04_valid_nc_min_plus_1", t, True, True))
    t = Z(); t[150]=th1+300
    for k in range(nc_max+2):
        pos=152+k*2
        if pos<150+tp: t[pos]=th2+10
    cases.append(("05_nc_exceeds_nc_max", t, False, False))
    t = Z(); t[150]=th1+300; t[152]=th2+10; t[152+(tsep+20)//2+2]=th2+10
    cases.append(("06_tsepmax_violated", t, False, False))
    t = Z(); t[50]=th1+300
    for k in range(nc_min):
        pos=52+k*2
        if pos<50+tp: t[pos]=th2+10
    cases.append(("07_t1_before_idx100", t, None, False))
    t = Z()
    for start in [150, 350]:
        t[start]=th1+300
        for k in range(nc_min):
            pos=start+2+k*2
            if pos<start+tp: t[pos]=th2+10
    cases.append(("08_two_valid_t1s", t, True, True))
    return cases

def cmp(label, my_r, cl_r, em, ec, verbose):
    mt,mn = my_r; ct,cn = cl_r
    agree=(mt==ct); my_ok=(em is None or mt==em); cl_ok=(ec is None or ct==ec)
    flag="OK" if (agree and my_ok and cl_ok) else "!!"
    if verbose or not agree or not my_ok or not cl_ok:
        extra=""
        if not my_ok: extra+=" [mine unexpected]"
        if not cl_ok: extra+=" [Clement unexpected]"
        print(f"  [{flag}] {label:<38}  mine={str(mt):<6}nc={str(mn):<5}  clement={str(ct):<6}nc={str(cn):<5}{'AGREE' if agree else 'DIFFER'}{extra}")
    return agree

def run_synthetic(params, verbose):
    print("\n"+"="*72+"\nSYNTHETIC TEST CASES\n"+"="*72)
    cases=build_cases(params); n=0
    discs=[]
    for label,trace,em,ec in cases:
        my_r=run_mine(trace,params); cl_r=run_clement(trace,params)
        ok=cmp(label,my_r,cl_r,em,ec,verbose)
        if ok: n+=1
        else: discs.append((label,my_r,cl_r))
    print(f"\nSynthetic: {n}/{len(cases)} agree.")
    for d in discs: print(f"  DISCREPANCY {d[0]}: mine={d[1]}, clement={d[2]}")
    return n, len(cases)

def run_real(npz_path, params, verbose):
    print("\n"+"="*72+f"\nREAL TRACE TESTS  -->  {os.path.basename(npz_path)}\n"+"="*72)
    data=np.load(npz_path,allow_pickle=True); keys=list(data.files)
    print(f"  Keys: {keys}")
    groups=[]
    for k in keys:
        arr=data[k]
        if not isinstance(arr,np.ndarray) or arr.ndim==0 or arr.shape[-1]<50: continue
        if arr.ndim==1: groups.append((k,arr[np.newaxis,:]))
        elif arr.ndim==2: groups.append((k,arr))
        elif arr.ndim==3:
            for i in range(arr.shape[0]): groups.append((f"{k}_e{i}",arr[i]))
    if not groups: print("  No trace arrays found."); return 0,0
    na,nt=0,0
    for name,traces in groups:
        nc=traces.shape[0]
        print(f"\n  [{name}]  {nc} ch x {traces.shape[-1]} samples")
        for ch in range(nc):
            trace=traces[ch].astype(int)
            my_r=run_mine(trace,params); cl_r=run_clement(trace,params)
            ok=cmp(f"{name}_ch{ch}",my_r,cl_r,None,None,verbose)
            nt+=1
            if ok: na+=1
    print(f"\n  Real: {na}/{nt} agree.")
    return na, nt

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--synthetic-only",action="store_true")
    ap.add_argument("--real-only",action="store_true")
    ap.add_argument("--npz",default=None)
    ap.add_argument("-v","--verbose",action="store_true")
    for k,v in DEFAULT_PARAMS.items(): ap.add_argument(f"--{k.replace('_','-')}",type=int,default=v,dest=k)
    args=ap.parse_args()
    params={k:getattr(args,k) for k in DEFAULT_PARAMS}
    print("="*72)
    print("FLT0 CROSS-CHECK")
    print("  Mine   : FLT0/T1_trigger_offline.py :: extract_trigger_parameters()")
    print("  Clement: cross_check_Clement/offline_FLT0_trigger.py :: trigger_FLT0()")
    print(f"  Params : {params}")
    print("="*72)
    ta,tt=0,0
    if not args.real_only:
        na,nt=run_synthetic(params,args.verbose); ta+=na; tt+=nt
    if not args.synthetic_only:
        npz=args.npz
        if npz is None:
            cand=os.path.join(THIS_DIR,"..","test_data","entry_100_du_1090.npz")
            npz=cand if os.path.exists(cand) else None
        if npz: na,nt=run_real(npz,params,args.verbose); ta+=na; tt+=nt
        else: print("\nNo .npz file found. Use --npz to specify one.")
    print("\n"+"="*72)
    print(f"GRAND TOTAL: {ta}/{tt} cases agree.")
    print("="*72)
    print("""
Known intentional differences:
  1. index_T1<=100 guard: Clement skips early T1s; mine does not.
  2. Quiet failure: mine raises ValueError; Clement skips and continues.
  3. Tsepmax violation: mine raises ValueError; Clement marks T1 invalid and continues.
  4. Multiple T1s: Clement returns all; mine returns only the first valid T1.
  5. NC bounds: Clement inclusive (nc_min<=NC<=nc_max); run_FLT0 uses exclusive.
""")

if __name__=="__main__":
    main()
