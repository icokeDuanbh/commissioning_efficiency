/**
 *Created by duanbh,
 *2022.4.22,
 *Thits .h is for the exiting foreign software, because we now use the version to do some work, when we have ours then we could update all the program.
 **/
#pragma once

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <functional>
#include <iostream>
#include <fstream>
#include <map>
#include <data_format.h>
#include <thread>
#include <mutex>

#include "frontend.h"

#include "template_FLT.h"
#include "tflite_inference.h"

#define DEVFILE "/dev/mem"
// #define Reg_End 0x1FC
#define Reg_End 0x2FC

namespace grand {

	class ScopeA: public IFrontend {
		public:
			ScopeA();
			~ScopeA();
		protected:
			virtual void elecInit();
			virtual void elecConfig(void *parameters);
			virtual void elecStartRun();
			virtual int elecReadData(char *data, size_t maxSize, uint32_t* hitId);
			virtual void elecStopRun();
			virtual void elecTerminate();

		private:
			void scopeRawWrite(uint32_t regAddr, uint32_t value);
			void scopeRawRead(uint32_t regAddr, uint32_t *value);
			void scopeSetParameter(uint32_t regAddr, uint32_t value, bool toShadow = false);
			void scopeFlush();

			std::mutex m_mtx;

			int station_id = 0;

			uint32_t m_pageOffset;
			int32_t m_dev = 0;
			void *m_axiPtr;
			// uint16_t* evtbuf;
			uint16_t m_ppsbuf[WCNT_PPS*GPSSIZE];
			uint32_t m_shadowList[Reg_End>>2];


			uint64_t m_time0, m_time1;
			int m_counts = 0;
			uint64_t m_duNanoSec;

			// ****** NUTRIG ****** //
			TemplateFLT *template_flt_x;
			TemplateFLT *template_flt_y;

			S_TFLite *cnn_flt;

			// float xThre=0.5, yThre=0.4;
			float xThre=0.4;

	};
}

