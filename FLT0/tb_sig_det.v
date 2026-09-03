`timescale 1ns / 1ps

// If global_parameters.v is not uploaded, we define these fallback macros
`ifndef Cmd_v_8
  `define Cmd_v_8 8
  `define cThreshold1 0
  `define cThreshold2 2
  `define cTPrev 4
  `define cTPeriod 5
  `define cTCMax 6
  `define cNCMax 7
  `define cNCMin 8
  `define cTrgOpt 9
`endif

module tb_sig_det;

    // Parameters
    parameter CHAN_NUM = 4'd1;

    // Inputs
    reg clk_adc;
    reg rst_na;
    reg [48*`Cmd_v_8-1:0] tparm_list_in;
    reg [15:0] fadc_in;
    reg rdout_rdy_in;

    // Outputs
    wire data_valid_out;
    wire [13:0] b_free_out;
    wire [8:0] nc_cnt_out;
    wire [13:0] pmax_out;
    wire trig_out;

    // Instantiation of the Unit Under Test (UUT)
    sig_det #(
        .CHAN_NUM(CHAN_NUM)
    ) uut (
        .clk_adc(clk_adc),
        .rst_na(rst_na),
        .tparm_list_in(tparm_list_in),
        .fadc_in(fadc_in),
        .rdout_rdy_in(rdout_rdy_in),
        .data_valid_out(data_valid_out),
        .b_free_out(b_free_out),
        .nc_cnt_out(nc_cnt_out),
        .pmax_out(pmax_out),
        .trig_out(trig_out)
    );

    // Register configuration helpers
    reg [13:0] p_threshold_t1;
    reg [13:0] p_threshold_t2;
    reg [7:0]  p_tprev;
    reg [7:0]  p_tperiod;
    reg [7:0]  p_tcmax;
    reg [7:0]  p_ncmax;
    reg [7:0]  p_ncmin;
    reg [7:0]  p_trig_opt;

    // Pack the parameters into tparm_list_in
    always @(*) begin
        tparm_list_in = 0;
        tparm_list_in[((CHAN_NUM-1)*12 + `cThreshold1)*`Cmd_v_8 +: 14] = p_threshold_t1;
        tparm_list_in[((CHAN_NUM-1)*12 + `cThreshold2)*`Cmd_v_8 +: 14] = p_threshold_t2;
        tparm_list_in[((CHAN_NUM-1)*12 + `cTPrev)*`Cmd_v_8      +: 8]  = p_tprev;
        tparm_list_in[((CHAN_NUM-1)*12 + `cTPeriod)*`Cmd_v_8    +: 8]  = p_tperiod;
        tparm_list_in[((CHAN_NUM-1)*12 + `cTCMax)*`Cmd_v_8      +: 8]  = p_tcmax;
        tparm_list_in[((CHAN_NUM-1)*12 + `cNCMax)*`Cmd_v_8      +: 8]  = p_ncmax;
        tparm_list_in[((CHAN_NUM-1)*12 + `cNCMin)*`Cmd_v_8      +: 8]  = p_ncmin;
        tparm_list_in[((CHAN_NUM-1)*12 + `cTrgOpt)*`Cmd_v_8     +: 8]  = p_trig_opt;
    end

    // Clock generation: 500 MHz (2ns period -> 1ns high / 1ns low)
    always #1 clk_adc = ~clk_adc;

    initial begin
        // Waveform dumping for EDA Playground (EPWave)
        $dumpfile("dump.vcd");
        $dumpvars(0, tb_sig_det);

        // Initialize Inputs
        clk_adc = 0;
        rst_na = 0;
        fadc_in = 0;
        rdout_rdy_in = 0;

        // Configure detector parameters
        p_threshold_t1 = 14'd500;   // Signal threshold T1
        p_threshold_t2 = 14'd100;   // Noise threshold T2
        p_tprev        = 8'd10;     // Minimum gap between T1 crossings (in clock cycles)
        p_tperiod      = 8'd20;     // Tper period (Tper_5x = 5 * 20 = 100 clock cycles)
        p_tcmax        = 8'd30;     // Max allowed gap between T2 crossings
        p_ncmax        = 8'd8;      // Max T2 crossings allowed
        p_ncmin        = 8'd2;      // Min T2 crossings required
        
        // TRIG_OPT settings:
        // bit 0: 0 = positive sign trigger, 1 = negative
        // bit 1: 0 = disable Pmax judgement (asserts trig_out directly)
        //        1 = enable Pmax judgement (asserts data_valid_out for external divider)
        p_trig_opt     = 8'b00000000; // Let's set bit1=0 to see trig_out directly!

        // Wait for global reset
        #10;
        rst_na = 1;
        
        // CRITICAL FIX: 
        // tc1_time_cnt counts cycles since last T1 crossing. At startup, it starts at 0.
        // The trigger logic requires tc1_time_cnt > p_tprev (10 cycles) to accept a trigger.
        // Therefore, we must wait at least 15 clock cycles after reset before sending a pulse!
        repeat (15) @(posedge clk_adc);

        // ----------------------------------------------------
        // CASE 1: Test a valid event (trig_opt[1]=0 -> trig_out should go high)
        // ----------------------------------------------------
        $display("Starting Case 1: Valid event (direct trig_out)");
        
        // 1. Initial T1 crossing (pulse start)
        @(posedge clk_adc);
        fadc_in = 16'd600; // Above T1 (500)
        
        @(posedge clk_adc);
        fadc_in = 16'd50;  // Below T2 (100)
        
        repeat (5) @(posedge clk_adc);

        // 2. Oscillate above T2 a few times (3 crossings total)
        fadc_in = 16'd200; // 2nd T2 crossing
        @(posedge clk_adc);
        fadc_in = 16'd50;
        
        repeat (5) @(posedge clk_adc);
        
        fadc_in = 16'd250; // 3rd T2 crossing
        @(posedge clk_adc);
        fadc_in = 16'd0;
        
        // Wait for Tper window to end (Tper_5x = 100 clock cycles)
        repeat (100) @(posedge clk_adc);
        
        // Acknowledge readout ready to clear state
        rdout_rdy_in = 1;
        repeat (2) @(posedge clk_adc);
        rdout_rdy_in = 0;
        repeat (20) @(posedge clk_adc);


        // ----------------------------------------------------
        // CASE 2: Test a valid event with Pmax enabled (trig_opt[1]=1 -> data_valid_out should go high)
        // ----------------------------------------------------
        $display("Starting Case 2: Valid event with Pmax (data_valid_out)");
        
        // Set trig_opt[1] = 1 (enable Pmax/divider mode)
        p_trig_opt = 8'b00000010;
        
        // Wait for parameter update
        repeat (5) @(posedge clk_adc);

        // 1. Initial T1 crossing (pulse start)
        fadc_in = 16'd600; // Above T1 (500)
        @(posedge clk_adc);
        fadc_in = 16'd50;  // Below T2 (100)
        repeat (5) @(posedge clk_adc);

        // 2. Oscillate above T2 (3 crossings total)
        fadc_in = 16'd200; // 2nd T2 crossing
        @(posedge clk_adc);
        fadc_in = 16'd50;
        repeat (5) @(posedge clk_adc);
        fadc_in = 16'd250; // 3rd T2 crossing
        @(posedge clk_adc);
        fadc_in = 16'd0;
        
        // Wait for Tper window to end (100 clock cycles)
        repeat (100) @(posedge clk_adc);
        
        // Acknowledge readout
        rdout_rdy_in = 1;
        repeat (2) @(posedge clk_adc);
        rdout_rdy_in = 0;
        repeat (20) @(posedge clk_adc);


        // ----------------------------------------------------
        // CASE 3: Noise with too many crossings (should NOT trigger)
        // ----------------------------------------------------
        $display("Starting Case 3: Noise with too many crossings");
        p_trig_opt = 8'b00000000; // Direct trigger mode
        repeat (5) @(posedge clk_adc);

        // 1. Initial T1 crossing
        fadc_in = 16'd600; 
        @(posedge clk_adc);
        fadc_in = 16'd0;
        repeat (5) @(posedge clk_adc);

        // Generate 10 crossings (exceeds nc_max = 8)
        repeat (10) begin
            fadc_in = 16'd150; // Above T2
            @(posedge clk_adc);
            fadc_in = 16'd0;   // Below T2
            repeat (3) @(posedge clk_adc);
        end
        
        repeat (100) @(posedge clk_adc);

        $display("Simulation finished.");
        $finish;
    end

endmodule
