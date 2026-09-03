# Simulation Results & Trigger Efficiency Analysis Pipeline

This document provides a summary of the analysis pipeline built to process simulation outputs (`.npz` files), evaluate trigger efficiencies, perform spectral analysis, and quantify uncertainties in instantaneous efficiency and integrated detector exposure.

---

## 1. Data Ingestion & Trigger Setup

- **Simulation Manifest (`sim_manifest.csv`)**:
  - Contains **14,950 simulated showers** across two primary cosmic ray species:
    - **Proton**: `primary_type = 14`
    - **Iron**: `primary_type = 5626`
  - Primary energies span $\log_{10}(E/\text{GeV}) \in [8.0, 11.0]$ ($\sim 100\text{ PeV}$ to $100\text{ EeV}$).
- **Background Noise Realizations**:
  - Each shower is injected into **2,323 background noise time traces** sampled at $f_s = 20\text{ Hz}$ ($\Delta t = 0.05\text{ s}$, covering $\sim 116\text{ seconds}$).
- **Trigger Condition (SLT)**:
  - Configurable via `--slt-tag` suffix (e.g., `triggered == True` marked as `SLT_SLT`, or `n_flt0_passed >= 5` marked as `SLT_FLT0`).

---

## 2. Code Structure & Workflow

```
results/
  ├── sim_manifest.csv
  └── sim_batch_XXXX/sim_YYYYY.npz
        │
        ├── plot_efficiency_vs_energy.py ────> Energy turn-on S-curve (in GeV)
        └── plot_efficiency_vs_time.py   ────> Saves efficiency_vs_time_SLT_{tag}.npz
              │                                  & plots 3 energy bands (>100PeV, <3EeV, ≥3EeV)
              │
              ├── plot_efficiency_fft.py ──────────────> FFT spectrum (< 1 Hz zoom)
              ├── compute_fractional_power.py ─────────> Power split above/below f_Nyq = 0.05 Hz
              ├── quantify_nrmse_downsampling.py ──────> Instantaneous NRMSE & Rel Error
              ├── compare_exposure.py ─────────────────> Cumulative exposure E(t) comparison
              └── evaluate_phase_exposure.py ──────────> Phase offset shift variance (200 phases)
```

---

## 3. Detailed Step-by-Step Analysis & Findings

### A. Efficiency vs. Energy (`plot_efficiency_vs_energy.py`)
- **Method**: For each shower $i$, computes time-averaged efficiency $\bar{\epsilon}_i = \frac{1}{T} \sum_j \text{trig}_i(t_j)$, then bins by primary energy $\log_{10}(E/\text{GeV}) \in [8.0, 11.0]$.
- **Finding**: Confirmed the characteristic sigmoid activation turn-on curve (near $0\%$ efficiency below $1\text{ EeV}$, rising smoothly to a near-$100\%$ plateau above $3\text{ EeV}$).

### B. Efficiency vs. Background Time (`plot_efficiency_vs_time.py`)
- **Method**: Evaluates time-dependent array efficiency $\epsilon(t_j) = \frac{\# \text{triggered showers}}{N_{\text{showers}}}$ at each timestamp $t_j$ across three energy bands ($>100\text{ PeV}$, $<3\text{ EeV}$, $\ge 3\text{ EeV}$).
- **Output**: Saves `results/efficiency_vs_time_SLT_{slt_tag}.npz` containing `rel_time_sec` and efficiency arrays for each band.

### C. FFT Spectral Analysis (`plot_efficiency_fft.py` & `compute_fractional_power.py`)
- **Sampling Parameters**: $f_s = 20\text{ Hz}$ ($d = 0.05\text{ s}$).
- **Target Downsampling**: For a target $10\text{ s}$ sampling interval ($\Delta t = 10\text{ s}$), the Nyquist frequency is $f_{\text{Nyq}} = \frac{1}{2 \times 10\text{s}} = 0.05\text{ Hz}$.
- **Power Ratio**:
  $$\frac{P(f \ge 0.05\text{ Hz})}{P_{\text{AC}}} \approx 85\%$$
- **Finding**: $85\%$ of the fluctuating AC power resides in high-frequency noise bursts ($f \ge 0.05\text{ Hz}$), which cannot be resolved by $10\text{ s}$ sampling.

### D. Instantaneous Efficiency Error (`quantify_nrmse_downsampling.py`)
- **Method**: Downsamples efficiency to $10\text{ s}$ steps, linearly interpolates back onto the $20\text{ Hz}$ grid, and evaluates error against original data.
- **Metrics**:
  - **NRMSE**: $\text{NRMSE} = \frac{\text{RMSE}}{\sigma_{\epsilon}} \approx 0.92 \quad (\approx 1.0)$
  - **Relative Error**: $\frac{\text{RMSE}}{\bar{\epsilon}} \approx 10\%$
- **Consistency Verification**: Mathematically consistent with spectral power: $\text{NRMSE} \approx \sqrt{P_{\text{HF}} / P_{\text{AC}}} = \sqrt{0.85} \approx 0.92$.
- **Takeaway**: Estimating *instantaneous* efficiency $\epsilon(t)$ with $10\text{ s}$ sampling has **$\sim 10\%$ relative uncertainty**.

### E. Integrated Exposure Accuracy (`compare_exposure.py` & `evaluate_phase_exposure.py`)
- **Method**: Evaluates cumulative exposure $E(t) = \int_0^t \epsilon(t') dt'$ for both $20\text{ Hz}$ full resolution and $10\text{ s}$ sampling across all 200 possible phase offsets ($t_0 \in [0, 9.95]\text{ s}$).
- **Physics Principle**: Integration acts as a low-pass filter with frequency response $H(f) \propto \frac{1}{\pi f}$. High-frequency positive and negative noise spikes cancel out during summation over the 2-minute window.
- **Takeaway**: Estimating the *accumulated exposure* over 2 minutes with $10\text{ s}$ sampling achieves **$\sim 1\%$ precision**.

---

## 4. Key Takeaways & Guidelines for Future Runs

| Quantity | Sampling Interval | Precision / Uncertainty | Physical Reason |
|---|---|---|---|
| **Instantaneous Efficiency $\epsilon(t)$** | $10\text{ s}$ ($0.1\text{ Hz}$) | **$\sim 10\%$ uncertainty** ($\text{NRMSE} \approx 1.0$) | High-frequency noise ($f > 0.05\text{ Hz}$) is unresolved. |
| **Integrated Exposure (2 minutes)** | $10\text{ s}$ ($0.1\text{ Hz}$) | **$\sim 1\%$ precision** | Positive and negative noise spikes cancel out during integration. |
| **Integrated Exposure (Months / Years)** | **$1$ to $3$ hours** | **$< 1\%$ precision** | Statistical noise integrates out; sampling rate limit is set by the **24-hour day/night systematic cycle**. |

---

## 5. Script Summary Reference

All scripts accept `--results-dir` (to specify batch folders) and `--slt-tag` (to tag SLT conditions in output filenames).

| Script Name | Input Files | Purpose | Output Files |
|---|---|---|---|
| [plot_efficiency_vs_energy.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/plot_efficiency_vs_energy.py) | `--manifest sim_manifest.csv`<br>`--results-dir sim_batch_XXXX/*.npz` | Plot SLT trigger efficiency vs. primary energy [GeV] | `{results-dir}/imgs/efficiency_vs_energy_slt_logspace.png` |
| [plot_efficiency_vs_time.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/plot_efficiency_vs_time.py) | `--manifest sim_manifest.csv`<br>`--results-dir sim_batch_XXXX/*.npz` | Plot efficiency vs. time across 3 energy bands | `{results-dir}/imgs/efficiency_vs_time_SLT_{slt_tag}.png`<br>`{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` |
| [plot_efficiency_fft.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/plot_efficiency_fft.py) | `{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` | Plot FFT spectrum of efficiency time series (< 1 Hz) | `{results-dir}/imgs/fft_efficiency_vs_time_SLT_{slt_tag}.png` |
| [compute_fractional_power.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/compute_fractional_power.py) | `{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` | Compute AC power ratio above/below $f_{\text{Nyq}} = 0.05\text{ Hz}$ | Terminal Summary Table |
| [quantify_nrmse_downsampling.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/quantify_nrmse_downsampling.py) | `{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` | Compute NRMSE & Rel Error vs. downsampling interval | `{results-dir}/imgs/nrmse_vs_sampling_interval.png`<br>`{results-dir}/imgs/efficiency_reconstruction_10s.png` |
| [compare_exposure.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/compare_exposure.py) | `{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` | Compare cumulative integrated exposure $E(t)$ | `{results-dir}/imgs/exposure_comparison_10s.png` |
| [evaluate_phase_exposure.py](file:///Users/xishui/Dropbox/Project/GRAND/Event_injector/evaluate_phase_exposure.py) | `{results-dir}/efficiency_vs_time_SLT_{slt_tag}.npz` | Test exposure variance across 200 sampling phases | `{results-dir}/imgs/exposure_vs_phase_offset.png`<br>`{results-dir}/imgs/exposure_phase_distribution.png` |
