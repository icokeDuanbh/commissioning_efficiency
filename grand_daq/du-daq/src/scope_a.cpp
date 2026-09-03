/*********************************************
 *Created by duanbh,
 *2022/4/22,
 *ScopeA stands for now foreign version software.
**********************************************/
#include "scope_a.h"
#include <cassert>
#include <stdint.h>
#include <utils.h>
#include <chrono>
#include <tuple>

using namespace grand;
using namespace std;

ScopeA::ScopeA()
{
// Initiate the TemplateFLT objects for X and Y channels for Template-Fitting-T2
// NOTE: If at some point you want to use Z, you need to include a TEMPLATE_Z_FILE
std::cout << "Loading templates for channel X..." << std::endl;
template_flt_x = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
std::cout << "Done." << std::endl;
std::cout << "Loading templates for channel Y..." << std::endl;
template_flt_y = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
std::cout << "Done." << std::endl;
// Initiate the TFlite object for CNN-T2
cnn_flt = TFLT_create(2); // 2 CPU cores using multithreading of TFlite
}

ScopeA::~ScopeA(){
}

void ScopeA::scopeRawRead(uint32_t regAddr, uint32_t *value) // new, reading from AXI
{
    // assert(("m_axiPtr should not be NULL", m_axiPtr != nullptr));
    *value = *((unsigned int *)((char *)m_axiPtr+m_pageOffset+regAddr));
}

void ScopeA::scopeRawWrite(uint32_t regAddr, uint32_t value)
{
    //printf("==>regAddr %x and %x\n", regAddr, (unsigned int *)((char *)m_axiPtr+m_pageOffset+regAddr));
  *((unsigned int *)((char *)m_axiPtr+m_pageOffset+regAddr)) = value;
}

void ScopeA::scopeFlush()
{
  assert(("m_axiPtr should not be NULL", m_axiPtr != nullptr));
  scopeRawWrite(Reg_GenControl, 0x08000000); // clear DAQ Fifo's
  scopeRawWrite(Reg_GenControl, 0x00000000);
}

void ScopeA::scopeSetParameter(uint32_t regAddr, uint32_t value, bool toShadow)
{
    assert(m_axiPtr != nullptr);
    if(toShadow) m_shadowList[regAddr>>2] = value;
    scopeRawWrite(regAddr,value);
    usleep(1000);
}

void ScopeA::elecInit() {
    unsigned int addr, page_addr;
    unsigned int page_size=sysconf(_SC_PAGESIZE);
    
    printf("open scope\n");
    if(m_dev != 0) close(m_dev);
    m_axiPtr = NULL;
    printf("Trying to open !%s!\n",DEVFILE);
    assert((m_dev = open(DEVFILE, O_RDWR)) != -1);
    m_dev = open(DEVFILE, O_RDWR);
    assert(("m_dev open failed", m_dev != -1));
    addr = (unsigned int) TDAQ_BASE;
    page_addr = (addr & ~(page_size-1));
    m_pageOffset = addr - page_addr;
    m_axiPtr=mmap(NULL,page_size,PROT_READ|PROT_WRITE, MAP_SHARED,m_dev,page_addr);
    if ((long)m_axiPtr == -1) {
        perror("opening scope\n");
        exit(-1);
    }
    //printf("==>m_axiPtr %x\n", (unsigned int *)((char *)m_axiPtr)); //add by xx
    printf("Done opening m_dev = %d\n",(int)m_dev);
    sleep(1);
    assert(("m_axiPtr should not be NULL",m_axiPtr != NULL));
    //scopeRawWrite(Reg_Dig_Control,0x00004000);
    scopeRawWrite(Reg_Dig_Control,0x00000000);
    memset(m_shadowList,0,sizeof(m_shadowList));
    assert(("m_axiPtr should not be NULL",m_axiPtr != NULL));
    scopeFlush();
    sleep(2);     // added by xx
}

void ScopeA::elecConfig(void *parameters) {
    memcpy(m_shadowList, parameters, Reg_End);
    uint32_t *m_shadowList = (uint32_t*)parameters;
    uint32_t value_read;

    for(int i=0;i<Reg_End;i+=4){
        scopeSetParameter(i, m_shadowList[i>>2]);
        printf("%x is set as %x\n", i, m_shadowList[i>>2]);
        
        // scopeRawRead(i,&value_read);
        // printf("%x read is %x\n", i, value_read);
        // assert(("value_read should equal value_set", *value_read = m_shadowList[i>>2]));
    }

    // while(1){
    // uint32_t isData;
    // scopeRawRead(Reg_GenStatus,&isData);
    // printf("==> isData:%x\n", isData);
    // sleep(1);
    // }
}

void ScopeA::elecStartRun() {
    scopeFlush();
    sleep(2);     // added by xx, to avoid GPS CTP error
    scopeSetParameter(Reg_Dig_Control,m_shadowList[Reg_Dig_Control>>2] |(CTRL_PPS_EN | CTRL_SEND_EN ), true);
    m_counts = 0;   // added by xx
}

int ScopeA::elecReadData(char *data, size_t maxSize, uint32_t* hitId){
    std::unique_lock<mutex> lock(m_mtx);

    assert(("m_axiPtr should not be NULL", m_axiPtr != NULL));
    uint32_t isData, tbuf, *pbuf, ctp;
    bool hasData = false;
    int ret = 0;
    int offset = 0;
    int32_t prevgps = 0;
    int32_t evgps=0; 
    int length;
    struct tm tt;
    double fracsec;
    uint32_t sec, nanosec;
    uint16_t* evtbuf;

    scopeRawRead(Reg_GenStatus,&isData);
    if((isData&(GENSTAT_EVTFIFO)) == 0) {

        uint32_t value_read;
        // scopeRawWrite(Reg_GenControl,GENCTRL_EVTREAD | GENCTRL_BASELINECOR_DISABLE | GENCTRL_USE_ROM);
        scopeRawWrite(Reg_GenControl,GENCTRL_EVTREAD);
        scopeRawRead(Reg_GenControl,&value_read);
        // data in buffer
        uint32_t dataSizeRaw;
        scopeRawRead(Reg_Data, &dataSizeRaw);
        // printf("==> First Data in package:%08x\n", dataSizeRaw);
        if((dataSizeRaw>>16) == 0xADC0) {
            uint32_t dataSizeBytes = (dataSizeRaw&0xffff)*sizeof(uint16_t);
            if(dataSizeBytes > maxSize) std::cout << "maxSize is " << maxSize << std::endl;  
            int dataSizeDWord = dataSizeBytes/(sizeof(uint32_t));
            assert(("data buffer size >= real data size", maxSize >= dataSizeBytes+sizeof(uint32_t) ));    
            uint32_t *ptr = (uint32_t*)data;
            *ptr++ = *hitId;
            (*hitId)++;
            printf("trace: %lld\n", *hitId);
            (*ptr++) = dataSizeRaw;

            evtbuf = new uint16_t[2*dataSizeDWord];
            evtbuf[2*dataSizeDWord] = {0};

            int n=0;
            for(int i=0; i<(dataSizeDWord-1); i++) {
                scopeRawRead(Reg_Data,ptr);
                //if (1){
                //    printf("Word num : %d, Data : %08x\n", i+1, *ptr);
                //}
                *(evtbuf+2*n) = *(ptr);
                *(evtbuf+2*n+1) = (*(ptr))>>16;
                n++;
                ptr++;
            }
            *(ptr-dataSizeDWord+1) = HEADER_EVT << 16;

            ElecEvent ev(evtbuf, dataSizeDWord*2);
            ev.getTimeNotFullDataSz(); // use getTimeNotFullDataSz because we do not include index 0 data.
            *(ptr-dataSizeDWord+2) = ev.getTimeNotFullDataSz().sec;
            *(ptr-dataSizeDWord+3) = ev.getTimeNotFullDataSz().nanosec;
            //printf("nanosec is %x\n", ev.getTimeNotFullDataSz().nanosec);
            
            uint64_t tmp;
            tmp = ev.getTimeNotFullDataSz().totalSec;
            ret = dataSizeBytes+sizeof(uint32_t);

            int triggerPattern = *(evtbuf+EVT_TRIG_PAT-2); 
            printf("triggerPattern: %d\n", triggerPattern);
            if(triggerPattern != 32 && triggerPattern != 64) {
                // // *************** T2 TRIGGER BLOCK **************** //
                // // *********** Pablo Correa,  2024-12-10 *********** //
                // Perform the T2 trigger algorithms
                tuple<uint16_t,uint16_t> t2_res = ev.scope_t2(template_flt_x,template_flt_y,cnn_flt);
                // Write the results of T2 into the data file
                // For now, we save it like:
                // ADC sample frequency = correlation value of template FLT
                // ADC resolution = score value of CNN FLT
                // This is so that GTOT will save the T2 results in the ROOT file
                printf("get 0: %d, get 1: %d\n", get<0>(t2_res), get<1>(t2_res));
                *(ptr-dataSizeDWord+7) = get<0>(t2_res) << 16;
                *(ptr-dataSizeDWord+7) += get<1>(t2_res);
                
                // // ****** NUTRIG selection ****** //
                // ****** Now xCorre will save the template value of the channel with maxmum amplitudes,yCorre will save the channle number ****** //
                float xCorre = float(get<0>(t2_res)) / 65535.0f;
                // float yCorre = float(get<1>(t2_res)) / 65535.0f;
                int yCorre = get<1>(t2_res);
                
                // if (xCorre < xThre || yCorre < yThre){
                if (xCorre < xThre) {
                    // printf("SKIP THIS TRACE, Channel: %d, Corre: %f\n", yCorre, xCorre);
                    ret = 0;
                }
            }
            // // *************** T2 TRIGGER BLOCK **************** //

            delete evtbuf;
            evtbuf = nullptr;
        }
        else {
            // dat in buffer format error
            // read data to empty
            CLOG(WARNING, "data") << "data format error 1";
            int readToEmptyLen = 100000;
            uint32_t tmpData;
            // FILE *fp;
            // fp = fopen("FPGA_Error.txt","w+");
            m_counts++;
            // fprintf(fp, "error counts: %d\n", m_counts);
            // fclose(fp);
            printf("error counts: %d\n", m_counts);
            do {
                //rate.add();
                scopeRawRead(Reg_Data, &tmpData);
                scopeRawRead(Reg_GenStatus, &isData);
                readToEmptyLen --;
                
                if((tmpData>>16) == 0xADC0) {
                    uint32_t dataSizeBytes = (tmpData&0xffff)*sizeof(uint16_t);
                    int dataSizeDWord = dataSizeBytes/(sizeof(uint32_t));
                    printf("error level 2 maxsize: %d\n", maxSize);
                    assert(("data buffer size >= real data size", maxSize >= dataSizeBytes+sizeof(uint32_t) ));    
                    uint32_t *ptr = (uint32_t*)data;
                    *ptr++ = *hitId;
                    (*hitId)++;
                    printf("hit: %lld\n", *hitId);
                    (*ptr++) = tmpData;

                    evtbuf = new uint16_t[2*dataSizeDWord];
                    evtbuf[2*dataSizeDWord] = {0};

                    int n=0;
                    for(int i=0; i<(dataSizeDWord-1); i++) {
                        scopeRawRead(Reg_Data,ptr);
                        *(evtbuf+2*n) = *(ptr);
                        *(evtbuf+2*n+1) = (*(ptr))>>16;
                        n++;
                        ptr++;
                    }
                    *(ptr-dataSizeDWord+1) = HEADER_EVT << 16;

                    ElecEvent ev(evtbuf, dataSizeDWord*2);
                    ev.getTimeNotFullDataSz(); // use getTimeNotFullDataSz because we do not include index 0 data.
                    *(ptr-dataSizeDWord+2) = ev.getTimeNotFullDataSz().sec;
                    *(ptr-dataSizeDWord+3) = ev.getTimeNotFullDataSz().nanosec;
                    //printf("nanosec is %x\n", ev.getTimeNotFullDataSz().nanosec);
                    uint64_t tmp;
                    tmp = ev.getTimeNotFullDataSz().totalSec;
                    // printf("nanotime is %lld\n", tmp);
                    ret = dataSizeBytes+sizeof(uint32_t);
                    delete evtbuf;
                    evtbuf = nullptr;
                    break;
                }
            } while((isData&(GENSTAT_EVTFIFO)) == 0 && readToEmptyLen > 0);
            
            CLOG(WARNING, "data") << "try read fifo to empty done";
        }
        //t1.stop();
        //CLOG(DEBUG, "data") << "CPU time of 1 event readout = " << t1.timeMs() << " ms";
    }

    return ret;
}

void ScopeA::elecStopRun() {
    printf("in elecStopRun: %08x\n", m_shadowList[Reg_Dig_Control>>2]);
    scopeSetParameter(Reg_Dig_Control, m_shadowList[Reg_Dig_Control>>2] & (~CTRL_PPS_EN & ~CTRL_SEND_EN ), true);
    scopeFlush();
    printf("Total error counts in this run : %d\n", m_counts);  // added by xx
}

void ScopeA::elecTerminate() {
    close(m_dev);
    m_dev = 0;
    m_axiPtr = NULL;
}

