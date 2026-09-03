import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer
import os

def pack_parameters(chan_num=1, threshold_t1=500, threshold_t2=100, tprev=10, tperiod=20, tcmax=30, ncmax=8, ncmin=2, trig_opt=0):
    """Packs the configuration parameters into a single 384-bit integer matching the Verilog design."""
    Cmd_v_8 = 8
    cThreshold1 = 0
    cThreshold2 = 2
    cTPrev = 4
    cTPeriod = 5
    cTCMax = 6
    cNCMax = 7
    cNCMin = 8
    cTrgOpt = 9
    
    val = 0
    offset = (chan_num - 1) * 12
    
    val |= (threshold_t1 & 0x3FFF) << ((offset + cThreshold1) * Cmd_v_8)
    val |= (threshold_t2 & 0x3FFF) << ((offset + cThreshold2) * Cmd_v_8)
    val |= (tprev & 0xFF)          << ((offset + cTPrev) * Cmd_v_8)
    val |= (tperiod & 0xFF)        << ((offset + cTPeriod) * Cmd_v_8)
    val |= (tcmax & 0xFF)          << ((offset + cTCMax) * Cmd_v_8)
    val |= (ncmax & 0xFF)          << ((offset + cNCMax) * Cmd_v_8)
    val |= (ncmin & 0xFF)          << ((offset + cNCMin) * Cmd_v_8)
    val |= (trig_opt & 0xFF)        << ((offset + cTrgOpt) * Cmd_v_8)
    
    return val

async def run_trigger_for_samples(dut, samples, threshold_t1=500, threshold_t2=100, tprev=10, tperiod=20, tcmax=30, ncmax=8, ncmin=2, trig_opt=0):
    """
    Helper function to load parameters, apply a list of samples,
    and check if a trigger (trig_out = 1) is detected.
    
    Returns:
        bool: True if a trigger was detected, False otherwise.
    """
    # 1. Apply parameters
    packed_params = pack_parameters(
        chan_num=1,
        threshold_t1=threshold_t1,
        threshold_t2=threshold_t2,
        tprev=tprev,
        tperiod=tperiod,
        tcmax=tcmax,
        ncmax=ncmax,
        ncmin=ncmin,
        trig_opt=trig_opt
    )
    dut.tparm_list_in.value = packed_params
    
    # 2. Reset the module to clear previous state
    dut.rst_na.value = 0
    await RisingEdge(dut.clk_adc)
    dut.rst_na.value = 1
    
    # Wait for the startup blanking timer (tc1_time_cnt must be > tprev)
    for _ in range(tprev + 5):
        await RisingEdge(dut.clk_adc)
        
    trigger_occurred = False
    
    # 3. Inject the input array (samples)
    for val in samples:
        await RisingEdge(dut.clk_adc)
        dut.fadc_in.value = val
        if dut.trig_out.value == 1:
            trigger_occurred = True

    # 4. Wait for the detection window to finish (Tper_5x = 5 * tperiod)
    # Plus some extra cycles to clear output hold
    for _ in range(5 * tperiod + 10):
        await RisingEdge(dut.clk_adc)
        if dut.trig_out.value == 1:
            trigger_occurred = True

    # 5. Handshake readout ready to release data hold
    dut.rdout_rdy_in.value = 1
    await RisingEdge(dut.clk_adc)
    dut.rdout_rdy_in.value = 0
    await RisingEdge(dut.clk_adc)

    return trigger_occurred

@cocotb.test()
async def run_trigger_simulation(dut):
    """Testbench for sig_det that reads parameters from env and input from file."""
    
    # Start the 500 MHz clock (2ns period)
    cocotb.start_soon(Clock(dut.clk_adc, 2, units="ns").start())
    
    # Read configuration parameters from environment variables (with default fallbacks)
    t1_val   = int(os.environ.get("THRESHOLD_T1", 500))
    t2_val   = int(os.environ.get("THRESHOLD_T2", 100))
    tprev    = int(os.environ.get("TPREV", 10))
    tperiod  = int(os.environ.get("TPERIOD", 20))
    tcmax    = int(os.environ.get("TCMAX", 30))
    ncmax    = int(os.environ.get("NCMAX", 8))
    ncmin    = int(os.environ.get("NCMIN", 2))
    trig_opt = int(os.environ.get("TRIG_OPT", 0))

    # Load input samples from file (from FADC_FILE env var or fallback to fadc_input.txt)
    input_file_path = os.environ.get("FADC_FILE", "fadc_input.txt")
    if not os.path.exists(input_file_path):
        dut._log.error(f"Input file '{input_file_path}' not found!")
        print("[RESULT] Triggered: Error - File not found")
        return
        
    with open(input_file_path, "r") as f:
        samples = []
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                samples.append(int(line))
                
    dut._log.info(f"Parameters loaded -> T1={t1_val}, T2={t2_val}, Tprev={tprev}, Tperiod={tperiod}, TCmax={tcmax}, NCmax={ncmax}, NCmin={ncmin}, TrigOpt={trig_opt}")
    dut._log.info(f"Injecting {len(samples)} samples from '{input_file_path}'...")
    
    # Run the simulation
    triggered = await run_trigger_for_samples(
        dut, 
        samples, 
        threshold_t1=t1_val, 
        threshold_t2=t2_val,
        tprev=tprev,
        tperiod=tperiod,
        tcmax=tcmax,
        ncmax=ncmax,
        ncmin=ncmin,
        trig_opt=trig_opt
    )
    
    # Print a structured result marker to stdout so the wrapper script can parse it easily
    print(f"\n[RESULT] Triggered: {triggered}\n")
