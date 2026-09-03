# GP Trigger Simulation

A simulation framework for the GRAND (Giant Radio Array for Neutrino Detection) trigger pipeline, implementing and testing the FLT0 → FLT1 → SLT trigger chain.

## Pipeline Overview

```
Raw ADC traces
      │
      ▼
 ┌─────────┐
 │ filters │  FIR low-pass + notch filtering (FPGA-equivalent)
 └─────────┘
      │
      ▼
 ┌──────┐
 │ FLT0 │  Level-0 trigger: signal detection (Verilog simulation via Python)
 └──────┘
      │
      ▼
 ┌──────┐
 │ FLT1 │  Level-1 trigger: event initiator (C++ shared library)
 └──────┘
      │
      ▼
 ┌─────┐
 │ SLT │  Software-level trigger: T3 pipeline (C++ shared library + binary)
 └─────┘
```

## Directory Structure

```
.
├── filters/                    # FIR filter design and Python implementation
│   ├── filters.py              # Filter functions
│   ├── fir_compiler_lp_120M.xci/.xml  # Xilinx FIR compiler IP core files
│   └── compare_filters.py
├── FLT0/                       # Level-0 trigger
│   ├── sig_det.v               # Verilog signal detection module
│   ├── tb_sig_det.v            # Testbench
│   ├── global_parameters.v     # Parameters
│   ├── run_FLT0.py             # Python runner
│   └── Makefile
├── FLT1/                       # Level-1 trigger
│   ├── event_initiator.cpp     # C++ source
│   ├── build_event_initiator.py  # Build script
│   └── run_FLT1.py             # Python runner
├── SLT/                        # Software-level trigger
│   ├── t3pipeline_initiator.cpp  # C++ source
│   ├── build_slt_pipeline.py   # Build script
│   └── run_SLT.py              # Python runner
├── add_sim_MD/                 # Simulation metadata helpers
├── grand_daq/                  # DAQ framework source
│   ├── du-daq/                 # DU DAQ (CMake)
│   ├── grand-daq/              # Grand DAQ (CMake)
│   └── template_flt_online/    # FLT online template
├── run_full_pipeline.py        # Run the complete trigger chain
├── run_batch_job.py            # Batch job launcher
└── pipeline_analysis_summary.md
```

## Dependencies

- Python ≥ 3.8
- NumPy, SciPy
- GCC or Clang (for building C++ components)
- Icarus Verilog or Verilator (for FLT0 Verilog simulation)
- CMake ≥ 3.15 (for `grand_daq` builds)

## Building Platform-Dependent Components

The compiled binaries (`.so` shared libraries and executables) are **not** tracked in this repository because they are platform-dependent. You must build them from source before running the pipeline.

### FLT1 — Event Initiator

```bash
python FLT1/build_event_initiator.py
# Produces: FLT1/libevent_initiator.so
```

### SLT — T3 Pipeline

```bash
python SLT/build_slt_pipeline.py
# Produces: SLT/libslt_pipeline.so
#           SLT/t3pipeline_initiator
```

## Running the Pipeline

### Full pipeline

```bash
python run_full_pipeline.py
```

### Individual stages

```bash
python FLT0/run_FLT0.py
python FLT1/run_FLT1.py
python SLT/run_SLT.py
```

### Batch jobs

```bash
python run_batch_job.py
```

## Contributing

Please open a Pull Request against the `main` branch. Direct pushes to `main` are disabled.

## Contact

For questions, contact the repository maintainer or open a GitHub Issue.
