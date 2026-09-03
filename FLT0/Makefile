# Makefile for cocotb simulation

# Simulator options (default to icarus verilog)
SIM ?= icarus
TOPLEVEL_LANG ?= verilog

# Verilog source files
VERILOG_SOURCES += $(PWD)/sig_det.v

# Top-level module name inside Verilog
TOPLEVEL = sig_det

# Name of the Python test module (test_sig_det.py)
MODULE = test_sig_det

# Use SystemVerilog/Verilog-2012 features in compiling Icarus Verilog
# We use COMPILE_ARGS instead of EXTRA_ARGS because -g2012 is a compilation-only flag
# and will cause errors if passed to the runtime engine (vvp).
COMPILE_ARGS += -g2012

include $(shell cocotb-config --makefiles)/Makefile.sim
