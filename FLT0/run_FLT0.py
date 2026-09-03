import os
import sys
import subprocess
import re
import tempfile
import threading
import shutil
import glob
from pathlib import Path

# Add FLT0 folder to sys.path to import T1_trigger_offline
FLT0_DIR = Path(__file__).resolve().parent
if str(FLT0_DIR) not in sys.path:
    sys.path.insert(0, str(FLT0_DIR))

try:
    from T1_trigger_offline import extract_trigger_parameters
except ImportError:
    from FLT0.T1_trigger_offline import extract_trigger_parameters


def clean_build_dirs():
    """Deletes all leftover sim_build* directories in FLT0."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    for path in glob.glob(os.path.join(script_dir, "sim_build*")):
        if os.path.isdir(path):
            shutil.rmtree(path, ignore_errors=True)


def run_trigger_py(trace, threshold_t1=500, threshold_t2=100,
                   tprev=10, tperiod=20, tcmax=30,
                   ncmax=8, ncmin=2, trig_opt=0):
    """
    Python trigger decision wrapper using extract_trigger_parameters from T1_trigger_offline.py.
    
    Returns True if extract_trigger_parameters succeeds and ncmin < NC < ncmax.
    Returns False if any trigger condition fails or raises ValueError.
    """
    trigger_config = {
        "th1": threshold_t1,
        "th2": threshold_t2,
        "t_quiet": tprev,
        "t_period": tperiod,
        "t_sepmax": tcmax,
        "nc_min": ncmin,
        "nc_max": ncmax,
    }

    try:
        dict_info = extract_trigger_parameters(trace, trigger_config)
        nc = dict_info.get("NC", 0)
        
        # Check if the number of T2 crossings falls within the given (ncmin, ncmax) range
        if ncmin < nc < ncmax:
            return True
        else:
            return False
    except ValueError:
        # Fails any criteria (no T1, quiet violation, Tsepmax violation, etc.)
        return False



def run_trigger_simulation(samples, threshold_t1=500, threshold_t2=100,
                            tprev=10, tperiod=20, tcmax=30,
                            ncmax=8, ncmin=2, trig_opt=0,
                            verbose=False, keep_sim_build=False):
    """
    Runs the HDL trigger simulation with the specified input samples and parameters.
    
    Args:
        samples (list of int or numpy array): ADC signal sample values.
        threshold_t1 (int): Signal trigger threshold.
        threshold_t2 (int): Noise crossings threshold.
        tprev (int): Minimum gap between T1 crossings (clock cycles).
        tperiod (int): Tper parameter (Tper_5x = 5 * tperiod window).
        tcmax (int): Max clock cycles allowed between T2 crossings.
        ncmax (int): Max T2 crossings allowed.
        ncmin (int): Min T2 crossings required.
        trig_opt (int): Trigger options bitmask.
        verbose (bool): If True, prints simulation output log.
        keep_sim_build (bool): If True, keeps the compiled sim_build directory. If False (default), removes temp build folders.
        
    Returns:
        bool: True if trigger fired, False if not.
        
    Raises:
        RuntimeError: If the simulation compilation or execution fails.
    """
    # 1. Create a process-isolated temporary file for the sample array
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as tf:
        temp_input_path = tf.name
        tf.write("# Dynamically generated sample data\n")
        for val in samples:
            tf.write(f"{int(val)}\n")

    pid = os.getpid()
    tid = threading.get_ident()
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sim_build_dir = os.path.join(script_dir, f"sim_build_{pid}_{tid}")

    try:
        # 2. Prepare environment variables to pass parameters to the testbench
        # NOTE: Add conversions from DAQ to the values used in FPGA
        env = os.environ.copy()
        env["FADC_FILE"] = temp_input_path
        env["THRESHOLD_T1"] = str(threshold_t1)
        env["THRESHOLD_T2"] = str(threshold_t2)
        env["TPREV"] = str(tprev // 2)
        env["TPERIOD"] = str(tperiod // 10)
        env["TCMAX"] = str(tcmax)
        env["NCMAX"] = str(ncmax)
        env["NCMIN"] = str(ncmin)
        env["TRIG_OPT"] = str(trig_opt)
        env["SIM_BUILD"] = sim_build_dir

        # CRITICAL FIX: Ensure the active Python's bin directory is in the PATH
        # This allows `make` to locate `cocotb-config` which is installed in the conda env.
        python_bin_dir = os.path.dirname(sys.executable)
        env["PATH"] = python_bin_dir + os.pathsep + env.get("PATH", "")

        # 3. Run the cocotb simulation via make in this script's directory
        result = subprocess.run(["make"], env=env, cwd=script_dir, capture_output=True, text=True)

        if verbose or os.environ.get("VERBOSE") == "1":
            print("--- SIMULATION STDOUT ---")
            print(result.stdout)

        if result.returncode != 0:
            print("--- SIMULATION STDERR ---")
            print(result.stderr)
            print("--- SIMULATION STDOUT ---")
            print(result.stdout)
            raise RuntimeError(f"Simulation failed with exit code {result.returncode}")

        # 4. Parse the output for the [RESULT] marker
        # Format expected: "[RESULT] Triggered: True" or "[RESULT] Triggered: False"
        match = re.search(r"\[RESULT\] Triggered:\s*(True|False)", result.stdout)
        if not match:
            raise RuntimeError("Could not find trigger result marker in simulation output.")

        # Convert match string to boolean
        triggered = (match.group(1) == "True")
        return triggered
    finally:
        # Clean up temporary input file after simulation finishes
        if os.path.exists(temp_input_path):
            os.remove(temp_input_path)
        # Clean up process build folder unless keep_sim_build is explicitly True
        if not keep_sim_build and os.path.exists(sim_build_dir):
            shutil.rmtree(sim_build_dir, ignore_errors=True)


if __name__ == "__main__":
    print("==================================================")
    print("Starting Trigger Parameter Sweep Experiment")
    print("==================================================")

    # Define a sample pulse signal
    # Initial T1 crossing, followed by 2 T2 crossings
    test_pulse = [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, # Quiet startup
        650, 0, 0, 0, 0,                             # Exceeds T1 (max is 650)
        200, 0, 0, 0, 0,                             # Exceeds T2 (200)
        250, 0, 0, 0, 0,                             # Exceeds T2 (250)
        0, 0, 0, 0, 0, 0, 0                          # Quiet cooldown
    ]

    print("\nStimulus Pulse Peak Value: 650 ADC Units")
    print("-" * 50)
    print(f"{'T1 Threshold':<15} | {'Trigger Fired?':<15}")
    print("-" * 50)

    # Sweep the T1 Threshold from 400 to 900
    for t1 in range(400, 1000, 100):
        try:
            # Run the trigger simulation
            is_triggered = run_trigger_simulation(
                samples=test_pulse,
                threshold_t1=t1,
                threshold_t2=150,
                ncmin=1,
                ncmax=3,
                tprev=255,
                verbose=0
            )
            print(f"{t1:<15} | {str(is_triggered):<15}")
        except Exception as e:
            print(f"Error at T1 = {t1}: {e}")

    print("-" * 50)
    print("Sweep complete.")
