#include "tflite_inference.h"
#include "data_format.h"
#include <utils.h>
#include <chrono>
// #include "/home/grand/externals/template_flt_online/s_tflite.h"

// #define CTP_value 499998552

using namespace grand;
using namespace std;

ElecEvent::ElecEvent(uint16_t *data, int sz)
{
  m_data = data;
  m_size = sz;
}

ElecEvent::~ElecEvent(){
    // delete m_data;
    // m_data = nullptr;
}

ElecEvent::s_time ElecEvent::getTimeNotFullDataSz()
{
    struct tm tt;
    double fracsec;
    uint32_t sec, nanosec;
    int offset = 0;
    uint32_t CTP_value;

    tt.tm_sec = (m_data[offset+EVT_STATSEC-2]&0xff)-m_data[offset+EVT_LEAP-2];    // Convert GPS in a number of seconds
    tt.tm_min = (m_data[offset+EVT_MINHOUR-2]>>8)&0xff;
    tt.tm_hour = (m_data[offset+EVT_MINHOUR-2])&0xff;
    tt.tm_mday = (m_data[offset+EVT_DAYMONTH-2]>>8)&0xff;
    tt.tm_mon = (m_data[offset+EVT_DAYMONTH-2]&0xff)-1;
    tt.tm_year = m_data[offset+EVT_YEAR-2] - 1900;
    sec = (unsigned int)timegm(&tt);

    if(*(m_data+36)!=0) {
        // fracsec = (double)(*(uint32_t*)(m_data+34))/(double)(*(uint32_t*)(m_data+36));
        CTP_value = *(uint32_t*)(m_data+36);
        CTP_value &= ~(1 << 31);
        fracsec = (double)(*(uint32_t*)(m_data+34))/(double)(CTP_value);
        nanosec = uint32_t(fracsec*1000000000ULL);
    }
    else {
        fracsec = 0;
    }
    s_time getTimeNotFullData_Sz;
    getTimeNotFullData_Sz.totalSec = (uint64_t)sec*1000000000 + (uint64_t)nanosec;
    getTimeNotFullData_Sz.sec = (uint64_t)sec;
    getTimeNotFullData_Sz.nanosec = (uint64_t)nanosec;

    return getTimeNotFullData_Sz;
}

ElecEvent::s_time ElecEvent::getTimeFullDataSz()
{
    struct tm tt;
    double fracsec;
    uint32_t sec, nanosec;
    int offset = 0;
    uint32_t CTP_value;
    
    tt.tm_sec = (m_data[offset+EVT_STATSEC+newDataSzAdded]&0xff)-m_data[offset+EVT_LEAP+newDataSzAdded];    // Convert GPS in a number of seconds
    tt.tm_min = (m_data[offset+EVT_MINHOUR+newDataSzAdded]>>8)&0xff;
    tt.tm_hour = (m_data[offset+EVT_MINHOUR+newDataSzAdded])&0xff;
    tt.tm_mday = (m_data[offset+EVT_DAYMONTH+newDataSzAdded]>>8)&0xff;
    tt.tm_mon = (m_data[offset+EVT_DAYMONTH+newDataSzAdded]&0xff)-1;
    tt.tm_year = m_data[offset+EVT_YEAR+newDataSzAdded] - 1900;
    sec = (unsigned int)timegm(&tt);
	
    if(*(m_data+EVT_CTP)!=0) {
        // fracsec = (double)(*(uint32_t*)(m_data+EVT_CTD+newDataSzAdded))/(double)(*(uint32_t*)(m_data+EVT_CTP+newDataSzAdded));
        CTP_value = *(uint32_t*)(m_data+EVT_CTP+newDataSzAdded);
        CTP_value &= ~(1 << 31);
        fracsec = (double)(*(uint32_t*)(m_data+EVT_CTD+newDataSzAdded))/(double)(CTP_value);
        nanosec = uint32_t(fracsec*1000000000ULL);
    }
    else {
        fracsec = 0;
    }

    s_time getTimeFullData_Sz;
    getTimeFullData_Sz.totalSec = (uint64_t)sec*1000000000 + (uint64_t)nanosec;
    getTimeFullData_Sz.sec = (uint64_t)sec;
    getTimeFullData_Sz.nanosec = (uint64_t)nanosec;

    return getTimeFullData_Sz;
}

/*
This is a temporary overload function for the development of the T2 trigger.
Contrary to the function above, which returns whether T2 triggers or not,
this function returns the result of both the template fit and the CNN methods.
The template fit output is a correlation value between [0,1].
The CNN output is a score value between [0,1].
*/
std::tuple<uint16_t,uint16_t> ElecEvent::scope_t2(TemplateFLT *template_flt_x, TemplateFLT *template_flt_y, S_TFLite *cnn_flt){
    // ***** Pablo Correa, 2024-12-10 *****

    // Get the number of samples for each channel
    // std::cout <<  "scope_t2" << std::endl;
    int n_samples_tot = m_data[EVT_TOT_SAMPLES-2]*16; // need to multiply by 16 here
    short n_samples_ch1 = m_data[EVT_CH1_SAMPLES-2];
    short n_samples_ch2 = m_data[EVT_CH2_SAMPLES-2];
    short n_samples_ch3 = m_data[EVT_CH3_SAMPLES-2];
    short n_samples_ch4 = m_data[EVT_CH4_SAMPLES-2];


    // Initiate Eigen::Array objects as input for the template FLT
    Eigen::ArrayXi adc_trace_ch1(n_samples_ch1);
    Eigen::ArrayXi adc_trace_ch2(n_samples_ch2);
    Eigen::ArrayXi adc_trace_ch3(n_samples_ch3);
    Eigen::ArrayXi adc_trace_ch4(n_samples_ch4);

    // Set some indices for the loop
    short start_idx_ch1 = EVT_ADC_DATA - 2;
    short start_idx_ch2 = start_idx_ch1 + n_samples_ch1;
    short start_idx_ch3 = start_idx_ch2 + n_samples_ch2;
    short start_idx_ch4 = start_idx_ch3 + n_samples_ch3;

    /* 
    Fill in the ADC traces from the event buffer
    For now just do all channels, easier for testing
    For template, cannot use the uint format for now, so need to do these loops
    More efficient would be to just copy memory using eg memcpy
    If you want to include that, need to adapt the template fitting code
    */
    
    // // Channel 1
    // int adc_idx_ch = 0;
    // for (int i = start_idx_ch1; i < start_idx_ch2; i++){
    //     adc_trace_ch1[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // // Channel 2
    // adc_idx_ch = 0;
    // for (int i = start_idx_ch2; i < start_idx_ch3; i++){
    //     adc_trace_ch2[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // // Channel 3
    // adc_idx_ch = 0;
    // for (int i = start_idx_ch3; i < start_idx_ch4; i++){
    //     adc_trace_ch3[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // // Channel 4
    // adc_idx_ch = 0;
    // for (int i = start_idx_ch4; i < start_idx_ch4 + n_samples_ch4; i++){
    //     adc_trace_ch4[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }

    // // Channel 1
    // int adc_idx_ch = 0;
    // for (int i = start_idx_ch1; i < start_idx_ch2; i++){
    //     adc_trace_ch1[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // // Channel 2
    // adc_idx_ch = 0;
    // int max_abs_idx_ch2 = 0;
    // int16_t max_abs_adc_ch2 = 0;
    // for (int i = start_idx_ch2; i < start_idx_ch3; i++){
    //     int16_t adc = static_cast<int16_t>(m_data[i]);
    //     adc_trace_ch2[adc_idx_ch] = adc;
    //     // adc_trace_ch2[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     if (std::abs(adc) > std::abs(max_abs_adc_ch2))
    //     {
    //         max_abs_adc_ch2 = std::abs(adc) ;
    //         max_abs_idx_ch2 = adc_idx_ch;
    //     }
    //     adc_idx_ch += 1;
    // }
    // // Channel 3
    // adc_idx_ch = 0;
    // int max_abs_idx_ch3 = 0;
    // int16_t max_abs_adc_ch3 = 0;
    // for (int i = start_idx_ch3; i < start_idx_ch4; i++){
    //     int16_t adc = static_cast<int16_t>(m_data[i]);
    //     adc_trace_ch3[adc_idx_ch] = adc;
    //     // adc_trace_ch3[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     if (std::abs(adc) > std::abs(max_abs_adc_ch3))
    //     {
    //         max_abs_adc_ch3 = std::abs(adc);
    //         max_abs_idx_ch3 = adc_idx_ch;
    //     }
    //     adc_idx_ch += 1;
    // }
    // // Channel 4
    // adc_idx_ch = 0;
    // for (int i = start_idx_ch4; i < start_idx_ch4 + n_samples_ch4; i++){
    //     adc_trace_ch4[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // Get trigger pattern 
    uint16_t triggerpattern =  *(m_data + EVT_TRIG_PAT-2);
    printf("trigger pattern in scope_t2: %d\n", triggerpattern);

    // Get some L1 trigger parameters
    // See the GP300_digitizer documentation (note there are some errors regarding the units in there, corrected for here)
    //int t_pre_coincidence_ch1 = m_data[EVT_WINDOWS-2]; // Pre-coincidence time window for Channel 1 [2 ns = ADC samples]
    int t_pre_coincidence_ch2 = m_data[EVT_WINDOWS-2 + 2]; // Pre-coincidence time window for Channel 1 [2 ns = ADC samples]
    int t_pre_coincidence_ch3 = m_data[EVT_WINDOWS-2 + 4]; // Pre-coincidence time window for Channel 1 [2 ns = ADC samples]
    //int t_pre_coincidence_ch4 = m_data[EVT_WINDOWS-2 + 6]; // Pre-coincidence time window for Channel 1 [2 ns = ADC samples]

    //int t_period_ch1 = (m_data[EVT_TRIGGER-2 + 2] >> 8)*5; // T_period for channel 1 [10 ns -> 2 ns = ADC samples]
    int t_period_ch2 = (m_data[EVT_TRIGGER-2 + 8] >> 8)*5; // T_period for channel 2 [10 ns -> 2 ns = ADC samples]
    int t_period_ch3 = (m_data[EVT_TRIGGER-2 + 14] >> 8)*5; // T_period for channel 3 [10 ns -> 2 ns = ADC samples]
    //int t_period_ch4 = (m_data[EVT_TRIGGER-2 + 20] >> 8)*5; // T_period for channel 4 [10 ns -> 2 ns = ADC samples]

    //int t_first_T1_crossing_ch1 = t_pre_coincidence_ch1 - t_period_ch1 - 88; // [2 ns = ADC samples]
    int t_first_T1_crossing_ch2 = t_pre_coincidence_ch2 - t_period_ch2 - 88; // [2 ns = ADC samples]
    int t_first_T1_crossing_ch3 = t_pre_coincidence_ch3 - t_period_ch3 - 88; // [2 ns = ADC samples]
    //int t_first_T1_crossing_ch4 = t_pre_coincidence_ch4 - t_period_ch4 - 88; // [2 ns = ADC samples]
    std::cout << "t_first_T1_crossing_ch2: " << t_first_T1_crossing_ch2 << std::endl;
    std::cout << "t_first_T1_crossing_ch3: " << t_first_T1_crossing_ch3 << std::endl;

    //  // Channel 1
    // int adc_idx_ch = 0;
    // for (int i = start_idx_ch1; i < start_idx_ch2; i++){
    //     adc_trace_ch1[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }

    // Channel 2
    int adc_idx_ch = 0;
    int max_abs_idx_ch2 = 0;
    int16_t max_abs_adc_ch2 = 0;
    std::cout << "t_first_T1_crossing_ch2: " << t_first_T1_crossing_ch2 << std::endl;
    for (int i = start_idx_ch2; i < start_idx_ch3; i++){
        int16_t adc = static_cast<int16_t>(m_data[i]);
        adc_trace_ch2[adc_idx_ch] = adc;
        // adc_trace_ch2[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
        if( t_first_T1_crossing_ch2 -10 < i < t_first_T1_crossing_ch2 + 30 ) {  // Get max around T1 crossing, for debug purpose only
            if (std::abs(adc) > std::abs(max_abs_adc_ch2))
            {
                max_abs_adc_ch2 = std::abs(adc) ;
                max_abs_idx_ch2 = adc_idx_ch;
            }
        }
        adc_idx_ch += 1;
    }

    // Channel 3
    adc_idx_ch = 0;
    int max_abs_idx_ch3 = 0;
    int16_t max_abs_adc_ch3 = 0;
    std::cout << "t_first_T1_crossing_ch3: " << t_first_T1_crossing_ch3 << std::endl;
    for (int i = start_idx_ch3; i < start_idx_ch4; i++){
        int16_t adc = static_cast<int16_t>(m_data[i]);
        adc_trace_ch3[adc_idx_ch] = adc;
        // adc_trace_ch3[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
        if( t_first_T1_crossing_ch3 - 10 < i < t_first_T1_crossing_ch3 + 30 ) {   // Get max around T1 crossing, for debug purpose only
            if (std::abs(adc) > std::abs(max_abs_adc_ch3))
            {
                max_abs_adc_ch3 = std::abs(adc) ;
                max_abs_idx_ch3 = adc_idx_ch;
            }
        }
        adc_idx_ch += 1;
    }

    // // Channel 4
    // adc_idx_ch = 0;
    // for (int i = start_idx_ch4; i < start_idx_ch4 + n_samples_ch4; i++){
    //     adc_trace_ch4[adc_idx_ch] = static_cast<int16_t>( m_data[i] ); // make sure to convert from uint16_t to int16_t!
    //     adc_idx_ch += 1;
    // }
    // DEBUG
    // cout << "t_pre_coincidence_ch1 = " << t_pre_coincidence_ch1 << ", t_pre_coincidence_ch2 = " << t_pre_coincidence_ch2 << ", t_pre_coincidence_ch3 = " << t_pre_coincidence_ch3 << ", t_pre_coincidence_ch4 = " << t_pre_coincidence_ch4 << std::endl;;
    // cout << "t_period_ch1 = " << t_period_ch1 << ", t_period_ch2 = " << t_period_ch2 << ", t_period_ch3 = " << t_period_ch3 << ", t_period_ch4 = " << t_period_ch4 << std::endl;;
    // cout << "t_first_T1_crossing_ch1 = " << t_first_T1_crossing_ch1 << ", t_first_T1_crossing_ch2 = " << t_first_T1_crossing_ch2 << ", t_first_T1_crossing_ch3 = " << t_first_T1_crossing_ch3 << ", t_first_T1_crossing_ch4 = " << t_first_T1_crossing_ch4 << std::endl;;
    
    // if (t_first_T1_crossing_ch1 < 0 || t_first_T1_crossing_ch1 >= n_samples_ch1){
	// std::cout << "WARNING: t_first_T1_crossing_ch1 = " << t_first_T1_crossing_ch1 << "is out of bounds! Returning (65534,65534)" << std::endl;
    //     return std::make_tuple(65534,65534);
    // }
    if (t_first_T1_crossing_ch2 < 0 || t_first_T1_crossing_ch2 >= n_samples_ch2){
        std::cout << "WARNING: t_first_T1_crossing_ch2 = " << t_first_T1_crossing_ch2 << "is out of bounds! Returning (65534,65534)" << std::endl;
        return std::make_tuple(65534,65534);
    }
    if (t_first_T1_crossing_ch3 < 0 || t_first_T1_crossing_ch3 >= n_samples_ch3){
        std::cout << "WARNING: t_first_T1_crossing_ch3 = " << t_first_T1_crossing_ch3 << "is out of bounds! Returning (65534,65534)" << std::endl;
        return std::make_tuple(65534,65534);
    }
    // if (t_first_T1_crossing_ch4 < 0 || t_first_T1_crossing_ch4 >= n_samples_ch4){
    //     std::cout << "WARNING: t_first_T1_crossing_ch4 = " << t_first_T1_crossing_ch4 << "is out of bounds! Returning (65534,65534)" << std::endl;
    //     return std::make_tuple(65534,65534);
    // }
    
    // Perform the FLT-1

    // ***** PERFORM TEMPLATE FLT *****
    int t_max_ch, max_adc_trace_ch;

    // ********** BLOCK FOR LPNHE TEST BENCH **********
    
    // Channel 1 = X
    // Find the position of the trace maximum within the time of first T1 crossing and the channel trigger
    

    // //std::cout << "t_first_T1_crossing_ch1: " << t_first_T1_crossing_ch1 << " , t_pre_coincidence_ch1: " <<  t_pre_coincidence_ch1 << std::endl;
    //max_adc_trace_ch = adc_trace_ch1.segment(t_first_T1_crossing_ch1,t_pre_coincidence_ch1).maxCoeff(&t_max_ch); // Yields sample of maximum in segment of trace
    //t_max_ch += t_first_T1_crossing_ch1; // To get the sample of maximum relative to the whole trace
    //if(t_max_ch > 900)  {
    //    printf("WARNING: t_max_ch: %d, jump !!", t_max_ch);
    //    return std::make_tuple(65534,65534);
    //}
    // Perform the template fit
    //template_flt_x->template_fit(adc_trace_ch1,t_max_ch);
    //std::cout << "Template FLT correlation (channel 1 = X) >>> rho = " << template_flt_x->corr_max_best << std::endl;

    // Channel 2 = Y
    // Find the position of the trace maximum within the time of first T1 crossing and the channel trigger
    //std::cout << "t_first_T1_crossing_ch2: " << t_first_T1_crossing_ch2 << " , t_pre_coincidence_ch2: " <<  t_pre_coincidence_ch2 << std::endl;
    //max_adc_trace_ch = adc_trace_ch2.segment(t_first_T1_crossing_ch2,t_pre_coincidence_ch2).maxCoeff(&t_max_ch); // Yields sample of maximum in segment of trace
    //t_max_ch += t_first_T1_crossing_ch2; // To get the sample of maximum relative to the whole trace
    //if(t_max_ch > 900)  {
    //    printf("WARNING: t_max_ch: %d, jump !!", t_max_ch);
    //    return std::make_tuple(65534,65534);
    //}
    // Perform the template fit'
    // std::cout << "adc_trace_ch2: " << adc_trace_ch2 << ", t_max_ch: " << t_max_ch << std::endl;
    //std::cout << "t_max_ch: " << t_max_ch << std::endl;
    //template_flt_y->template_fit(adc_trace_ch2,t_max_ch);
    //std::cout << "Template FLT correlation (channel 2 = Y) >>> rho = " << template_flt_y->corr_max_best << std::endl;;

   // ********** BLOCK FOR GP300 **********
    
    // Channel 2 = X
    // Find the position of the trace maximum within the time of first T1 crossing and the channel trigger
    //std::cout << "t_first_T1_crossing_ch1: " << t_first_T1_crossing_ch1 << " , t_pre_coincidence_ch1: " <<  t_pre_coincidence_ch1 << std::endl;
    //max_adc_trace_ch = adc_trace_ch1.segment(t_first_T1_crossing_ch1,t_pre_coincidence_ch1).maxCoeff(&t_max_ch); // Yields sample of maximum in segment of trace
    
    // ****** use channels value(2, 3) to be the final****** //
    printf("CH2 peak ADC = %d, pos = %d\n", max_abs_adc_ch2, max_abs_idx_ch2);
    printf("CH3 peak ADC = %d, pos = %d\n", max_abs_adc_ch3, max_abs_idx_ch3);
    printf("Trigger pattern = %d, \n", triggerpattern);
    
    //chSwitch = (max_abs_adc_ch2 > max_abs_adc_ch3) ? 2 : 3;
    
    // Perform the template fit
    if(triggerpattern == 512){
        t_max_ch = t_first_T1_crossing_ch2; // To get the sample of maximum relative to the whole trace
        if(t_max_ch > 900)  {
            printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
            return std::make_tuple(65534,65534);
        }
        
        template_flt_x->template_fit(adc_trace_ch2,t_max_ch);
        std::cout << "Template FLT correlation (channel 2 = X) >>> rho = " << template_flt_x->corr_max_best << std::endl;
        corrx_float = template_flt_x->corr_max_best;
        corrx_uint16 = static_cast<uint16_t>( corrx_float*65535 + 0.5 );
        corry_uint16 = 2;
    }

    else if(triggerpattern == 1024){
        t_max_ch = t_first_T1_crossing_ch3; // To get the sample of maximum relative to the whole trace
        if(t_max_ch > 900)  {
            printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
            return std::make_tuple(65534,65534);
        }
        // Perform the template fit'
        // std::cout << "adc_trace_ch2: " << adc_trace_ch2 << ", t_max_ch: " << t_max_ch << std::endl;
        //std::cout << "t_max_ch: " << t_max_ch << std::endl;
        template_flt_y->template_fit(adc_trace_ch3,t_max_ch);
        std::cout << "Template FLT correlation (channel 3 = Y) >>> rho = " << template_flt_y->corr_max_best << std::endl;
        corry_float = template_flt_y->corr_max_best;
        corrx_uint16 = static_cast<uint16_t>( corry_float*65535 + 0.5 );
        corry_uint16 = 3;
    }
    else{ // Both X and Y triggered
        // First compute correlation factor for Ch2 = X
        t_max_ch = t_first_T1_crossing_ch2; // To get the sample of maximum relative to the whole trace
        if(t_max_ch > 900)  {
            printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
            return std::make_tuple(65534,65534);
        }
        
        template_flt_x->template_fit(adc_trace_ch2,t_max_ch);
        std::cout << "Template FLT correlation (channel 2 = X) >>> rho = " << template_flt_x->corr_max_best << std::endl;
        
        // Now for Ch2 = Y
        t_max_ch = t_first_T1_crossing_ch3; // To get the sample of maximum relative to the whole trace
        if(t_max_ch > 900)  {
            printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
            return std::make_tuple(65534,65534);
        }
        // Perform the template fit'
        template_flt_y->template_fit(adc_trace_ch3,t_max_ch);
        std::cout << "Template FLT correlation (channel 3 = Y) >>> rho = " << template_flt_y->corr_max_best << std::endl;
                std::cout << "Template FLT correlation (channel 3 = Y) >>> rho = " << template_flt_y->corr_max_best << std::endl;
        
        if(template_flt_x->corr_max_best > template_flt_y->corr_max_best ){
            corrx_float = template_flt_x->corr_max_best;
            corrx_uint16 = static_cast<uint16_t>( corrx_float*65535 + 0.5 );
            corry_uint16 = 2;
        }
        else{
            corry_float = template_flt_y->corr_max_best;
            corrx_uint16 = static_cast<uint16_t>( corry_float*65535 + 0.5 );
            corry_uint16 = 3;
        }
    }
    // t_max_ch = t_first_T1_crossing_ch2; // To get the sample of maximum relative to the whole trace
    // if(t_max_ch > 900)  {
    //     printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
    //     return std::make_tuple(65534,65534);
    // }

    
    // template_flt_x->template_fit(adc_trace_ch2,t_max_ch);
    // std::cout << "Template FLT correlation (channel 2 = X) >>> rho = " << template_flt_x->corr_max_best << std::endl;

    // // Channel 3 = Y
    // // Find the position of the trace maximum within the time of first T1 crossing and the channel trigger
    // //std::cout << "t_first_T1_crossing_ch3: " << t_first_T1_crossing_ch2 << " , t_pre_coincidence_ch2: " <<  t_pre_coincidence_ch2 << std::endl;
    // //max_adc_trace_ch = adc_trace_ch2.segment(t_first_T1_crossing_ch2,t_pre_coincidence_ch2).maxCoeff(&t_max_ch); // Yields sample of maximum in segment of trace
    // t_max_ch = t_first_T1_crossing_ch3; // To get the sample of maximum relative to the whole trace
    // if(t_max_ch > 900)  {
    //     printf("WARNING: t_max_ch: %d, jump !!\n", t_max_ch);
    //     return std::make_tuple(65534,65534);
    // }
    // // Perform the template fit'
    // // std::cout << "adc_trace_ch2: " << adc_trace_ch2 << ", t_max_ch: " << t_max_ch << std::endl;
    // //std::cout << "t_max_ch: " << t_max_ch << std::endl;
    // template_flt_y->template_fit(adc_trace_ch3,t_max_ch);
    // std::cout << "Template FLT correlation (channel 3 = Y) >>> rho = " << template_flt_y->corr_max_best << std::endl;

    // Final result of the template FLT is the maximum of all correlation values
    // if(chSwitch == 2) {
    //     corrx_float = template_flt_x->corr_max_best;
    //     corrx_uint16 = static_cast<uint16_t>( corrx_float*65535 + 0.5 );
    //     corry_uint16 = chSwitch;
    // }
        
    // if(chSwitch == 3){
    //     corry_float = template_flt_y->corr_max_best;
    //     corrx_uint16 = static_cast<uint16_t>( corry_float*65535 + 0.5 );
    //     corry_uint16 = chSwitch;
    // }
        
    // float corrx_float = template_flt_x->corr_max_best;
    // float corry_float = template_flt_y->corr_max_best;
    //float corr_float = std::max(template_flt_x->corr_max_best,template_flt_y->corr_max_best);
    //std::cout << "Final template FLT correlation (max of X and Y) >>> rho = " << corr_float << std::endl;;

    // ***** PERFORM CNN FLT *****

    static float score_float = -1.0;
//#ifdef REAL_DU
//    if (n_samples_ch1 == TFLT_SAMPLE_IN_TRACE && n_samples_ch2 == TFLT_SAMPLE_IN_TRACE && n_samples_ch3 == TFLT_SAMPLE_IN_TRACE){
        // Fill in the CNN 3D trace
        
//        for (int i=0; i<n_samples_ch1; i+=3){
//            cnn_flt->a_3dtraces[i] = adc_trace_ch1[i] / 8192.0; // divide by 2**13 to normalize the trace between [-1,1]
//            cnn_flt->a_3dtraces[i+1] = adc_trace_ch2[i] / 8192.0; // divide by 2**13 to normalize the trace between [-1,1]
//            cnn_flt->a_3dtraces[i+2] = adc_trace_ch3[i] / 8192.0; // divide by 2**13 to normalize the trace between [-1,1]
//        }
//        // Perform the inference
//        TFLT_inference(cnn_flt,&score_float);
//        std::cout << "CNN FLT >>> score = " << score_float << std::endl;;
//    }
//    else{
//        std::cout << "Trigger ConvNet is defined for " << TFLT_SAMPLE_IN_TRACE << " samples ONLY. No CNN FLT evaluation." << std::endl;;
//    }
//#endif

    // ********** BLOCK FOR LPNHE TEST BENCH **********
    

    // *** BLOCK FOR GP300 *** 
    // Channel 2 = X
    // Channel 3 = Y
    // Channel 4 = Z
    // *** BLOCK FOR GP300 *** 

    
    // We will want to save the FLT results into empty/unused words of the data file
    // Cast float range of [0,1] to uint16 range of [0,2**16]
    
    // uint16_t corrx_uint16 = static_cast<uint16_t>( corrx_float*65535 + 0.5 ); // +0.5 to cast to nearest integer
    // uint16_t corry_uint16 = static_cast<uint16_t>( corry_float*65535 + 0.5 ); // +0.5 to cast to nearest integer
    
    //float tmp1 = corrx_float*65535 + 0.5;
    //float tmp2 = corry_float*65535 + 0.5;
    //uint16_t score_uint16 = static_cast<uint16_t>( score_float*65535 + 0.5 ); // +0.5 to cast to nearest integer
    //printf("corrx_float: %f, corry_float: %f\n", corrx_float, corry_float);
    //printf("corrx_float step2: %f, corry_float step2: %f\n", tmp1, tmp2);
    //printf("corrx_uint16: %d, corry_uint16: %d\n", corrx_uint16, corry_uint16);
    // std::cout <<  "scope_t2 end" << std::endl;
    
    return std::make_tuple(corrx_uint16,corry_uint16);
}