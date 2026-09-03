/*=================created by duanbh==================*/
/*====================2022.1.19=======================*/

#pragma once

#include<sys/time.h>
#include <stdint.h>

#ifdef Fake
#define MSGSTOR 10 //!< number of messages in shared memory
#else
#define MSGSTOR 100 //!< number of messages in shared memory
#endif
#define MAXMSGSIZE 1000 //!< max number of shorts to be sent from socket to the scope at once
#define DU_PORT 5001 //!< socket port number of the detector unit software

#define BUFSIZE 3000            //!< store up to 3000 events in circular buffer

#define DATA_MAX_SAMP   8192                       //!< Maximal trace length (samples)
#define MAX_READOUT     (256 + DATA_MAX_SAMP*4) //!< Maximal raw event size

//#define MAXT3 200               //!< 200 T3 events in circular cuffer

#define TDAQ_BASE             0x80000000
/* Register Definitions*/
#define Reg_Dig_Control       0x000
#define Reg_Trig_Enable       0x002
#define Reg_TestPulse_ChRead  0x004
#define Reg_Time_Common       0x006
#define Reg_Inp_Select        0x008
#define Reg_Spare_A           0x00A
#define Reg_Spare_B           0x00C
#define Reg_Spare_C           0x00E
#define Reg_Time1_Pre         0x010
#define Reg_Time1_Post        0x012
#define Reg_Time2_Pre         0x014
#define Reg_Time2_Post        0x016
#define Reg_Time3_Pre         0x018
#define Reg_Time3_Post        0x01A
#define Reg_Time4_Pre         0x01C
#define Reg_Time4_Post        0x01E
#define Reg_ADC1_Gain         0x020
#define Reg_ADC1_IntOff       0x022
#define Reg_ADC1_BaseMa       0x024
#define Reg_ADC1_BaseMi       0x026
#define Reg_ADC1_SpareA       0x028
#define Reg_ADC1_SpareB       0x02A
#define Reg_ADC2_Gain         0x02C
#define Reg_ADC2_IntOff       0x02E
#define Reg_ADC2_BaseMax      0x030
#define Reg_ADC2_BaseMin      0x032
#define Reg_ADC2_SpareA       0x034
#define Reg_ADC2_SpareB       0x036
#define Reg_ADC3_Gain         0x038
#define Reg_ADC3_IntOff       0x03A
#define Reg_ADC3_BaseMax      0x03C
#define Reg_ADC3_BaseMin      0x03E
#define Reg_ADC3_SpareA       0x040
#define Reg_ADC3_SpareB       0x042
#define Reg_ADC4_Gain         0x044
#define Reg_ADC4_IntOff       0x046
#define Reg_ADC4_BaseMax      0x048
#define Reg_ADC4_BaseMin      0x04A
#define Reg_ADC4_SpareA       0x04C
#define Reg_ADC4_SpareB       0x04E
#define Reg_Trig1_ThresA      0x050
#define Reg_Trig1_ThresB      0x052
#define Reg_Trig1_Times       0x054
#define Reg_Trig1_Tmax        0x056
#define Reg_Trig1_Nmin        0x058
#define Reg_Trig1_Qmin        0x05A
#define Reg_Trig2_ThresA      0x05C
#define Reg_Trig2_ThresB      0x05E
#define Reg_Trig2_Times       0x060
#define Reg_Trig2_Tmax        0x062
#define Reg_Trig2_Nmin        0x064
#define Reg_Trig2_Qmin        0x066
#define Reg_Trig3_ThresA      0x068
#define Reg_Trig3_ThresB      0x06A
#define Reg_Trig3_Times       0x06C
#define Reg_Trig3_Tmax        0x06E
#define Reg_Trig3_Nmin        0x070
#define Reg_Trig3_Qmin        0x072
#define Reg_Trig4_ThresA      0x074
#define Reg_Trig4_ThresB      0x076
#define Reg_Trig4_Times       0x078
#define Reg_Trig4_Tmax        0x07A
#define Reg_Trig4_Nmin        0x07C
#define Reg_Trig4_Qmin        0x07E
#define Reg_FltA1_A1          0x080
#define Reg_FltA1_A2          0x082
#define Reg_FltA1_B1          0x084
#define Reg_FltA1_B2          0x086
#define Reg_FltA1_B3          0x088
#define Reg_FltA1_B4          0x08A
#define Reg_FltA1_B5          0x08C
#define Reg_FltA1_B6          0x08E
#define Reg_FWStatus          0x1C0
#define Reg_GenStatus         0x1D0
#define Reg_GenControl        0x1D4
#define Reg_Data              0x1D8
#define Reg_TestTrace         0x1DC
#define Reg_Rate              0x1E0
// #define Reg_End               0x1FC
#define Reg_End               0x2FC

/*----------------------------------------------------------------------*/
/* PPS definition */
#define MAGIC_PPS     0xFACE
#define WCNT_PPS      32
#define PPS_MAGIC       1
#define PPS_TRIG_PAT    2
#define PPS_TRIG_RATE   3
#define PPS_CTD         4
#define PPS_CTP         6
#define PPS_OFFSET      8
#define PPS_LEAP       10
#define PPS_STATFLAG   11
#define PPS_CRITICAL   12
#define PPS_WARNING    13
#define PPS_YEAR       14
#define PPS_DAYMONTH   15
#define PPS_MINHOUR    16
#define PPS_STATSEC    17
#define PPS_LONGITUDE  18
#define PPS_LATITUDE   22
#define PPS_ALTITUDE   26
#define PPS_TEMPERATURE 30

	/* Control register bits */
#define CTRL_SEND_EN    (1 << 0)
#define CTRL_PPS_EN     (1 << 1)

#define GENSTAT_PPSFIFO  (1<<24)
#define GENSTAT_EVTFIFO  (1<<25)
#define GENCTRL_EVTREAD  (1<<25)
#define GENCTRL_BASELINECOR_DISABLE (1<<24)
#define GENCTRL_USE_ROM (1<<26)

// defined by duanbh
#define DU_ID 38

/*
  buffer definitions for the scope readout process.
 */
#define GPSSIZE 35              //!< buffer upto 35 GPS seconds info in circular buffer
#define DEV_READ_BLOCK 100      //!< fpga Device read blocksize, in Bytes

/*----------------------------------------------------------------------*/
	/*the routines*/
int scope_open();
int scope_read(int ioff);

/*----------------------------------------------------------------------*/
/* Event definition */
#define MAGIC_EVT         0xADC0
#define HEADER_EVT        256
#define FORMAT_EVT        1
#define EVT_LENGTH        0 // nr of int16 words
#define EVT_ID            1 // nr of int16 words
#define EVT_HARDWARE      2
#define EVT_HDRLEN        3 //256 (int16 words in the header)
#define EVT_SECOND        4
#define EVT_NANOSEC       6
#define EVT_TRIGGERPOS    8
#define EVT_T3FLAG        9
#define EVT_ATM_TEMP      17
#define EVT_ATM_PRES      18
#define EVT_ATM_HUM       19
#define EVT_ACCEL_X       20
#define EVT_ACCEL_Y       21
#define EVT_ACCEL_Z       22
#define EVT_BATTERY       23
#define EVT_VERSION       24
#define EVT_MSPS          25
#define EVT_ADC_RES       26
#define EVT_INP_SELECT    27
#define EVT_CH_ENABLE     28
#define EVT_TOT_SAMPLES   29
#define EVT_CH1_SAMPLES   30
#define EVT_CH2_SAMPLES   31
#define EVT_CH3_SAMPLES   32
#define EVT_CH4_SAMPLES   33
#define EVT_TRIG_PAT      34
#define EVT_TRIG_RATE     35
#define EVT_CTD           36
#define EVT_CTP           38
#define EVT_PPS_OFFSET    40
#define EVT_LEAP          42
#define EVT_GPS_STATFLAG  43
#define EVT_GPS_CRITICAL  44
#define EVT_GPS_WARNING   45
#define EVT_YEAR          46
#define EVT_DAYMONTH      47
#define EVT_MINHOUR       48
#define EVT_STATSEC       49
#define EVT_LONGITUDE     50
#define EVT_LATITUDE      54
#define EVT_ALTITUDE      58
#define EVT_GPS_TEMP      62
#define EVT_CTRL          64
#define EVT_WINDOWS       72
#define EVT_CHANNEL       80
#define EVT_TRIGGER       104
#define EVT_FILTER1       128
#define EVT_FILTER2       160
#define EVT_FILTER3       192
#define EVT_FILTER4       224

#define EVENT_BCNT        2 //bytecount
#define EVENT_TRIGMASK    4
#define EVENT_GPS         6
#define EVENT_STATUS     13 
#define EVENT_CTD        14
#define EVENT_LENCH1     18
#define EVENT_LENCH2     20
#define EVENT_LENCH3     22
#define EVENT_LENCH4     24
#define EVENT_THRES1CH1  26
#define EVENT_THRES2CH1  28
#define EVENT_THRES1CH2  30
#define EVENT_THRES2CH2  32
#define EVENT_THRES1CH3  34
#define EVENT_THRES2CH3  36
#define EVENT_THRES1CH4  38
#define EVENT_THRES2CH4  40
#define EVENT_CTRL       42
#define EVENT_WINDOWS    54
#define EVENT_ADC        70

/*----------------------------------------------------------------------*/
/* PPS definition */
#define MAGIC_PPS     0xFACE
#define WCNT_PPS      32

// next: what did we read from the scope?
#define SCOPE_PARAM 1          //!< return code for reading a parameter list
#define SCOPE_EVENT 2          //!< return code for reading an event
#define SCOPE_GPS   3          //!< return code for reading a PPS message

#define MAX_INP_MSG 20000 //!< maximum size (in shorts) of the input data (should be big enough for the configuation data)
#define MAX_OUT_MSG 50000 //!< maximum size (in shorts) of the output data (should be big enough for the T2 data/ a complete event)

#define MAX_T2 1000 //! max size for T2's, corresponds to the Adaq

#define EVT_ADC_DATA      256 // start of ADC data channel 1

typedef struct
{
  uint32_t ts_seconds;
  uint32_t ts_nanoseconds;
  uint16_t event_nr;
  uint16_t trigmask;
}TS_DATA; //timestamps

void scope_create_memory();
void scope_set_parameters(uint32_t reg_addr, uint32_t value,uint32_t to_shadow);
void scope_raw_write(uint32_t reg_addr, uint32_t value);
int32_t scope_raw_read(uint32_t reg_addr, uint32_t *value);
int scope_open();
void scope_get_parameterlist(uint8_t list);
void scope_reset();
void scope_start_run();
void scope_stop_run();
int scope_no_run_read();
int scope_run_read();
void readfromparas();
void scope_copy_shadow();
int scope_read_event();
void scope_close();
