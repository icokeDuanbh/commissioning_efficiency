#pragma once
#include <iostream>
#include <string.h>
#include <scope.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include "template_FLT.h"
#include "tflite_inference.h"
#include <tuple>

#define newDataSzAdded 2
#define TEMPLATES_XY_FILE "/home/root/grand-daq/arm/template_lib/templates_ZHAireS_DC2.1rc4_RAW_100_5.txt"

namespace grand{

  class ElecEvent {
  public:
    ElecEvent(uint16_t *data, int sz);
    ~ElecEvent();

    bool scope_t2_template(float threshold);
    std::tuple<uint16_t, uint16_t> scope_t2(
      TemplateFLT *template_flt_x,
      TemplateFLT *template_flt_y,
      S_TFLite *cnn_flt
    );

   

    //uint32_t getEvID();
    //uint32_t getSize();
    //uint32_t getLongitude();
  typedef struct 
  {
    uint32_t sec;
    uint32_t nanosec;
    uint64_t totalSec;
  }s_time;
    
    s_time getTimeNotFullDataSz();
    s_time getTimeFullDataSz();
  private:
    uint32_t m_size;
    uint16_t *m_data;
     
    // ****** BOHAO ****** //
    int chSwitch;
    float corrx_float, corry_float;
    uint16_t corrx_uint16, corry_uint16;
  };
  
  class DaqEvent {
    
  };
}

