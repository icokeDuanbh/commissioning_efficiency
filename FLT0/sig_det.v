`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 01/25/2025 12:10:40 PM
// Design Name: 
// Module Name: sig_det
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


`include "./global_parameters.v"

module sig_det
#(
    parameter CHAN_NUM = 4'd1
)
(
    input           clk_adc,
    input           rst_na,
    input   [48*`Cmd_v_8-1:0]   tparm_list_in,
    input   [15:0]  fadc_in,
    input           rdout_rdy_in,
    output          data_valid_out,
    output  [13:0]  b_free_out, //not used
    output  [8:0]   nc_cnt_out,
    output  [13:0]  pmax_out,
    output          trig_out
    );

reg     tcmax_reached;          //High when the TC2_TIMECNT has been reached TCMAX
reg     tper_run;               //High when TPER_COUNTER runs
reg     tper_run_r;
reg     [11:0]  tper_counter;   //Counts the valid measure time
reg     [8:0]   nc_cnt, nc_cnt_r;         //Counts the T2 threshold crossings
reg     [8:0]   tc1_time_cnt;   //Counts time between the T1-threshold crossings
reg     [8:0]   tc2_time_cnt;   //Counts time between the T2-threshold crossings
reg     data_valid;


reg     discr_t1;               //Discriminator; High when ADC data bigger than THRESHOLD_T1
reg     discr_t1_r;
reg     discr_t2;               //Discriminator; High when ADC data bigger than THRESHOLD_T2
reg     discr_t2_r;
reg     data_hold;              //Holds the data at the output until READOUT_READY
reg     nc_gt_ncmax;
reg     nc_in_range;
reg     tper_max_reached;

wire    [11:0]  tper_1x;                //1 times TPER; scale TPER_nX to 12 bits to make it summable
wire    [11:0]  tper_4x;                //4 times TPER
reg     [11:0]  tper_5x;                //5 times TPER

reg     [13:0]  pmax_val;               //Peakvalue of signal during TPER_RUN
reg     [13:0]  pmax_tmp;               //Peakvalue of signal during TPER_RUN?
reg     [13:0]  adc_value;
reg     adc_sign;

reg     [13:0]  d_threshold_t1;
reg     [13:0]  d_threshold_t2;
reg     [13:0]  d_threshold_t1_r;
reg     [13:0]  d_threshold_t2_r;
reg     [8:0]  nc_max;
reg     [8:0]  nc_min;
reg     [8:0]  tc_max;
reg     [8:0]  tper;                    //period
reg     [8:0]  tprev;                   //previous
reg     [7:0]  trig_opt;                //bit0: 0 to trigger on positive ADC signal, 1 for negative; other bits are not used.
                                        //bit1: 0 to disable Pmax judgement, 1 to enable Pmax use. (231226.xx)
wire    trig_allow;                     //trigger allowed if ADCsign same as TrgOpt(0)
wire    trig_wo_pmax;                   //generate trigger without pmax in consideration
reg     trig_sig;

assign  trig_allow = (trig_opt[0]==adc_sign) ? 1'b1 : 1'b0;
assign  b_free_out = adc_value;         //baseline free absolute value (positive, 0-8191)
assign  pmax_out = pmax_tmp ;           //peak values during TPER_RUN period. (positive, 0-8191)

assign  tper_1x[11:0] = {3'b000, tper[8:0]} ;    //TPER_nX
assign  tper_4x[11:0] = {1'b0, tper[8:0], 2'b00} ;
assign  nc_cnt_out = nc_cnt_r;
assign  data_valid_out = data_valid;
assign  trig_wo_pmax = ~trig_opt[1];
assign  trig_out = trig_sig;

// get the parameters from the list of registers, and put all of them in clk_adc domain
//always @(posedge clk_adc or negedge rst_na) begin
always @(posedge clk_adc) begin
//    if (!rst_na) begin
//        d_threshold_t1 <= 14'b0;            //-- signal threshold
//        d_threshold_t2 <= 14'b0;            // - noise threshold
//        tprev <= 9'b0;
//        tper <= 9'b0;
//        tc_max <= 9'b0;
//        nc_max <= 9'b0;
//        nc_min <= 9'b0;
//        trig_opt <= 8'b0;                   //trigger options (pos/neg edge)
//    end
//    else begin
        d_threshold_t1 <=        tparm_list_in[((CHAN_NUM-1)*12+`cThreshold1)*`Cmd_v_8+13:((CHAN_NUM-1)*12+`cThreshold1)*`Cmd_v_8];
        d_threshold_t2 <=        tparm_list_in[((CHAN_NUM-1)*12+`cThreshold2)*`Cmd_v_8+13:((CHAN_NUM-1)*12+`cThreshold2)*`Cmd_v_8];
        tprev          <= {1'b0, tparm_list_in[((CHAN_NUM-1)*12+`cTPrev)*`Cmd_v_8+7      :((CHAN_NUM-1)*12+`cTPrev)*`Cmd_v_8     ]};    // extend to 9 bits
        tper           <= {1'b0, tparm_list_in[((CHAN_NUM-1)*12+`cTPeriod)*`Cmd_v_8+7    :((CHAN_NUM-1)*12+`cTPeriod)*`Cmd_v_8   ]};    // --
        tc_max         <= {1'b0, tparm_list_in[((CHAN_NUM-1)*12+`cTCMax)*`Cmd_v_8+7      :((CHAN_NUM-1)*12+`cTCMax)*`Cmd_v_8     ]};    // --
        nc_max         <= {1'b0, tparm_list_in[((CHAN_NUM-1)*12+`cNCMax)*`Cmd_v_8+7      :((CHAN_NUM-1)*12+`cNCMax)*`Cmd_v_8     ]};    // --
        nc_min         <= {1'b0, tparm_list_in[((CHAN_NUM-1)*12+`cNCMin)*`Cmd_v_8+7      :((CHAN_NUM-1)*12+`cNCMin)*`Cmd_v_8     ]};    // --
        trig_opt       <=        tparm_list_in[((CHAN_NUM-1)*12+`cTrgOpt)*`Cmd_v_8+7     :((CHAN_NUM-1)*12+`cTrgOpt)*`Cmd_v_8    ];
//    end
end

//put in register for better timing in comparisom (TPER_COUNTER >= TPER_5X)
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        tper_5x <= 12'b0;
        d_threshold_t1_r <= 14'b0;
        d_threshold_t2_r <= 14'b0;
    end
    else begin
      tper_5x <= (tper_1x+tper_4x);
      d_threshold_t1_r <= d_threshold_t1;               // is this neccessary ??
      d_threshold_t2_r <= d_threshold_t2;
    end
end

//Change the input range from "2-s complement notation" to absolute (positive) value
//The FADC value is 2000, 2001 ... 3fff, 0000, 0001 ... 1fff; (the 0-Volt value is 0000).
//Absolute value is 0000, 1fff ... 0001, 0000, 0001 ... 1fff; (the 0-Volt value is 0000).
//need check for AD9694
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        adc_value <= 14'b0;
        adc_sign <= 1'b0;
    end
    else begin
        if (fadc_in[13]==1'b0) begin                        // positive
            adc_value <= {1'b0, fadc_in[12:0]};
            adc_sign <= 1'b0;
        end
        else begin                                          // negative
            adc_value <= {1'b0, (~fadc_in[12:0]+13'b1)};
            adc_sign <= 1'b1;
        end
    end
end

//discr_t1 : 1'b1 indicates adc_value higher than T1
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        discr_t1 <= 1'b0;
        discr_t1_r <= 1'b0;
    end
    else begin
        discr_t1_r <= discr_t1;
        if ((trig_allow==1'b1) && (adc_value > d_threshold_t1_r))
            discr_t1 <= 1'b1;
        else
            discr_t1 <= 1'b0;
    end
end

//discr_t2
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        discr_t2 <= 1'b0;
        discr_t2_r <= 1'b0;
    end
    else begin
        discr_t2_r <= discr_t2;
        if ((trig_allow==1'b1) && (adc_value > d_threshold_t2_r))
            discr_t2 <= 1'b1;
        else
            discr_t2 <= 1'b0;
    end
end

//tc1_time_cnt : counts time between T1 threshold crossings
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        tc1_time_cnt <= 9'b0;
    end
    else begin
        if ((discr_t1==1'b1) && (discr_t1_r == 1'b0)) begin     // every time the ADC data goes over the T1 (high) threshold
            tc1_time_cnt <= 9'b0;                               // reset TC1_TIMECNT
        end
        else if (tc1_time_cnt == 9'b1_1111_1111) begin          // if TC1_TIMECNT is full
            tc1_time_cnt <= tc1_time_cnt;                       // lock TC1_TIMECNT
        end
        else begin
            tc1_time_cnt <= tc1_time_cnt + 1'b1;                // increase TC1_TIMECNT
        end
    end
end

//tc2_time_cnt : counts time between T2 threshold crossings while TPER running
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        tc2_time_cnt <= 9'b0;
    end
    else begin
        if(tper_run==1'b1)  begin
            if ((discr_t2==1'b1) && (discr_t2_r == 1'b0)) begin     // every time the ADC data goes over the T2 (high) threshold
                tc2_time_cnt <= 9'b0;                               // reset TC2_TIMECNT
            end
            else if (tc2_time_cnt == 9'b1_1111_1111) begin          // if TC2_TIMECNT is full
                tc2_time_cnt <= tc2_time_cnt;                       // lock TC2_TIMECNT
            end
            else begin
                tc2_time_cnt <= tc2_time_cnt + 1'b1;                // increase TC2_TIMECNT
            end
        end
        else begin
            tc2_time_cnt <= 9'b0;                                   // not in tper, reset TC2_TIMECNT
        end
    end
end

// tcmax_reached : TCMAX has been reached by (T2) TC2_TIMECNT
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        tcmax_reached <= 1'b0;
    end
    else begin
        if ((tper_run == 1'b1) && (tc2_time_cnt >= tc_max))
            tcmax_reached <= 1'b1;              //if TPER_COUNTER is running and TC2_TIMECNT higher than TCMAX
        else
            tcmax_reached <= 1'b0;
    end
end

// nc_cnt : number of T2 threshold crossings
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        nc_cnt <= 9'b0;
        nc_in_range <= 1'b0;
        nc_gt_ncmax <= 1'b0;
    end
    else begin
        // ----
        if (tper_run == 1'b1) begin                             // if TPER_COUNTER is running
            if (tper_run_r == 1'b0)                             // the first crossing is at TPER_RUN
                nc_cnt <= 9'b1;                                 // initial value (1)
            else if (nc_cnt == 9'b1_1111_1111)                  // if NC_COUNTER is full
                nc_cnt <= nc_cnt;                               // lock NC_COUNTER
            else if ((discr_t2==1'b1) && (discr_t2_r==1'b0))    // if there is a T2 (noise) threshold crossing
                nc_cnt <= nc_cnt+1'b1;                          // increase NC_COUNTER
        end
        else begin
            nc_cnt <= 9'b0;                                     // reset nc_cnt
        end
        // ----
        if ((nc_cnt > nc_min) && (nc_cnt < nc_max))
            nc_in_range <= 1'b1;
        else
            nc_in_range <= 1'b0;
        // ----
        if (nc_cnt > nc_max)
            nc_gt_ncmax <= 1'b1;
        else
            nc_gt_ncmax <= 1'b0;
    end
end

// TPER_RUN : set when we are checking whether the last T1 crossing is valid
//-- if TPER_COUNTER is higher than the setting of TPER_5X
//-- OR { the maximum T2 threshold crossing time (gap) has been reached
//--     AND there is another T2 threshold crossing within TPER_5X }
//-- OR there have been too many T2 threshold crossings
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
         tper_run <= 1'b0;
         tper_run_r <= 1'b0;
    end
    else begin
        tper_run_r <= tper_run;
        if (tper_max_reached==1'b1) begin
            tper_run <= 1'b0;
        end
        else if (((tcmax_reached==1'b1)&&(discr_t2==1'b1)&&(discr_t2_r==1'b0))
                || (nc_gt_ncmax==1'b1)) begin
            tper_run <= 1'b0;             // Reset TPER_RUN
        end
        else if ((tc1_time_cnt>tprev)&&(discr_t1==1'b1)&&(discr_t1_r==1'b0)) begin
        //if TC1_TIMECNT bigger than TPREV and there is a T1 threshold crossing
            tper_run <= 1'b1;             //Set TPER_RUN
        end
    end
end

// tper_counter
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        tper_counter <= 12'b0;
    end
    else begin
        if (tper_run ==1'b1) begin
            if (tper_counter==12'b0100_1111_1011)
                tper_counter <= tper_counter;           // if TPER_COUNTER is full (5*255= 1275 = x04FB). lock
            else
                tper_counter <= tper_counter + 1'b1;    // Increase TPER_COUNTER
        end
        else begin
            tper_counter <= 12'b0;                      // Reset TPER_COUNTER
        end
    end
end

//tper_max_reached
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na)
        tper_max_reached <= 1'b0;
    else begin
        if ((tper_run==1'b1)&&(tper_counter>=tper_5x))
            tper_max_reached <= 1'b1;
        else
            tper_max_reached <= 1'b0;
    end
end

//pmax_val : store the max value of adc in tper
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        pmax_val <= 14'b0;
    end
    else begin
        if (tper_run==1'b1) begin
            if (adc_value > pmax_val)
                pmax_val <= adc_value;
            else
                pmax_val <= pmax_val;
        end
        else begin
            pmax_val <= 14'b0;
        end
    end
end

//data_valid
//data_hold
always @(posedge clk_adc or negedge rst_na) begin
    if (!rst_na) begin
        data_hold <= 1'b0;
        data_valid <= 1'b0;
        pmax_tmp <= 14'b0;
        nc_cnt_r <= 9'b0;
        trig_sig <= 1'b0;
    end
    else  begin
        if (rdout_rdy_in ==1'b1) begin
            data_hold <= 1'b0;
            trig_sig <= trig_sig;
        end
        else if ((data_hold==1'b0) && (tper_max_reached==1'b1) && (nc_in_range==1'b1)) begin
            if (trig_wo_pmax==1'b1) begin
                data_valid <= 1'b0;
                data_hold <= 1'b0;
                pmax_tmp <= 14'b0;
                nc_cnt_r <= 9'b0;
                trig_sig <= 1'b1;
            end
            else begin
                data_valid <= 1'b1;                     // start divider
                data_hold <= 1'b1;                      // hold values
                pmax_tmp <= pmax_val;                   // max. adc values
                nc_cnt_r <= nc_cnt;                     // number of crossing = peak width
                trig_sig <= 1'b0;
            end
        end
        else begin
            data_valid <= 1'b0;
            data_hold <= data_hold;
            pmax_tmp <= pmax_tmp;
            nc_cnt_r <= nc_cnt_r;
            trig_sig <= 1'b0;
        end
    end
end


endmodule
