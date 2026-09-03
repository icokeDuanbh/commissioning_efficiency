#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include "../grand_daq/du-daq/src/data_format.h"

// #define TEST_MAIN
#define TEMPLATES_XY_FILE "grand_daq/du-daq/src/template_flt_online/templates_ZHAireS_DC2.1rc4_RAW_XY_THRESH_100_5.txt"

// Struct for the legacy record header in the .dat file (12 bytes)
struct LegacyRecordHeader {
    uint32_t size;   // Total size of the record including this header
    uint32_t type;   // Record type (e.g., 1 for DU event)
    uint32_t source; // DU ID
};

/**
 * Creates a mock ElecEvent buffer by borrowing header structure from a legacy .dat file,
 * and optionally injecting custom input trace arrays for Channels 1, 2, 3, and 4.
 *
 * @param filepath Path to the .dat file.
 * @param record_index The 0-based index of the record to load as the baseline template.
 * @param custom_trace_ch1 Custom trace for Channel 1 (nullptr to keep original).
 * @param len_ch1 Length of custom trace for Channel 1.
 * @param custom_trace_ch2 Custom trace for Channel 2 (X) (nullptr to keep original).
 * @param len_ch2 Length of custom trace for Channel 2.
 * @param custom_trace_ch3 Custom trace for Channel 3 (Y) (nullptr to keep original).
 * @param len_ch3 Length of custom trace for Channel 3.
 * @param custom_trace_ch4 Custom trace for Channel 4 (Z) (nullptr to keep original).
 * @param len_ch4 Length of custom trace for Channel 4.
 * @param out_size Output parameter for the size of the returned buffer in uint16_t words.
 * @return A dynamically allocated uint16_t array containing the event payload, or nullptr on failure.
 */
uint16_t* create_mock_event_from_dat(
    const char* filepath,
    int record_index,
    const int16_t* custom_trace_ch1,
    int len_ch1,
    const int16_t* custom_trace_ch2,
    int len_ch2,
    const int16_t* custom_trace_ch3,
    int len_ch3,
    const int16_t* custom_trace_ch4,
    int len_ch4,
    int& out_size
) {
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Failed to open file " << filepath << std::endl;
        return nullptr;
    }

    // Skip the 256-byte file header
    ifs.seekg(256, std::ios::beg);

    LegacyRecordHeader rec_hdr;
    int current_idx = 0;

    while (ifs.read(reinterpret_cast<char*>(&rec_hdr), sizeof(LegacyRecordHeader))) {
        uint32_t payload_size = rec_hdr.size - sizeof(LegacyRecordHeader);

        if (current_idx == record_index) {
            std::vector<char> payload(payload_size);
            if (!ifs.read(payload.data(), payload_size)) {
                std::cerr << "Error: Truncated payload for record " << record_index << std::endl;
                return nullptr;
            }

            if (payload_size < 8) {
                std::cerr << "Error: Payload size is too small." << std::endl;
                return nullptr;
            }

            // Extract hitId and dataSizeRaw from payload prefix
            uint32_t hitId = *reinterpret_cast<uint32_t*>(payload.data());
            uint32_t dataSizeRaw = *reinterpret_cast<uint32_t*>(payload.data() + 4);
            uint32_t dataSizeWords = dataSizeRaw & 0xFFFF;

            // Compute size of evtbuf
            int evt_words = (payload_size - 8) / sizeof(uint16_t);
            out_size = dataSizeWords > evt_words ? dataSizeWords : evt_words;

            // Allocate and initialize evtbuf
            uint16_t* evtbuf = new uint16_t[out_size]();
            std::memcpy(evtbuf, payload.data() + 8, payload_size - 8);

            // Read channel samples configurations (offsets are shifted by -2 in m_data)
            short n_samples_ch1 = evtbuf[30 - 2];
            short n_samples_ch2 = evtbuf[31 - 2];
            short n_samples_ch3 = evtbuf[32 - 2];
            short n_samples_ch4 = evtbuf[33 - 2];

            // ADC start offset in evtbuf (EVT_ADC_DATA = 256)
            int start_idx_ch1 = 256 - 2;
            int start_idx_ch2 = start_idx_ch1 + n_samples_ch1;
            int start_idx_ch3 = start_idx_ch2 + n_samples_ch2;
            int start_idx_ch4 = start_idx_ch3 + n_samples_ch3;

            // Inject Channel 1 trace if provided
            if (custom_trace_ch1 && len_ch1 > 0) {
                int copy_len = len_ch1 > n_samples_ch1 ? n_samples_ch1 : len_ch1;
                std::memcpy(evtbuf + start_idx_ch1, custom_trace_ch1, copy_len * sizeof(uint16_t));
            }

            // Inject Channel 2 trace if provided
            if (custom_trace_ch2 && len_ch2 > 0) {
                int copy_len = len_ch2 > n_samples_ch2 ? n_samples_ch2 : len_ch2;
                std::memcpy(evtbuf + start_idx_ch2, custom_trace_ch2, copy_len * sizeof(uint16_t));
            }

            // Inject Channel 3 trace if provided
            if (custom_trace_ch3 && len_ch3 > 0) {
                int copy_len = len_ch3 > n_samples_ch3 ? n_samples_ch3 : len_ch3;
                std::memcpy(evtbuf + start_idx_ch3, custom_trace_ch3, copy_len * sizeof(uint16_t));
            }

            // Inject Channel 4 trace if provided
            if (custom_trace_ch4 && len_ch4 > 0) {
                int copy_len = len_ch4 > n_samples_ch4 ? n_samples_ch4 : len_ch4;
                std::memcpy(evtbuf + start_idx_ch4, custom_trace_ch4, copy_len * sizeof(uint16_t));
            }

            return evtbuf;
        }

        ifs.seekg(payload_size, std::ios::cur);
        current_idx++;
    }

    std::cerr << "Error: Record index " << record_index << " not found." << std::endl;
    return nullptr;
}

/**
 * Creates a mock ElecEvent buffer entirely in memory from scratch (without a .dat file).
 *
 * @param trigger_pattern The trigger pattern configuration (Word 34 in header).
 * @param t_pre_coincidence_ch2 Pre-coincidence window for Channel 2.
 * @param t_pre_coincidence_ch3 Pre-coincidence window for Channel 3.
 * @param t_period_reg_ch2 Trigger period register value for Channel 2.
 * @param t_period_reg_ch3 Trigger period register value for Channel 3.
 * @param custom_trace_ch1 Custom trace for Channel 1.
 * @param len_ch1 Length of Channel 1 trace.
 * @param custom_trace_ch2 Custom trace for Channel 2.
 * @param len_ch2 Length of Channel 2 trace.
 * @param custom_trace_ch3 Custom trace for Channel 3.
 * @param len_ch3 Length of Channel 3 trace.
 * @param custom_trace_ch4 Custom trace for Channel 4.
 * @param len_ch4 Length of Channel 4 trace.
 * @param out_size Output parameter for the size of the returned buffer in uint16_t words.
 * @return A dynamically allocated uint16_t array containing the event.
 */
uint16_t* create_mock_event_from_scratch(
    uint16_t trigger_pattern, // Which channel is triggered, deciding which channel is used in the FLT1
    uint16_t t_pre_coincidence_ch2, // unit: ns
    uint16_t t_pre_coincidence_ch3, // unit: ns
    uint16_t t_period_reg_ch2, // unit: ns
    uint16_t t_period_reg_ch3, // unit: ns
    const int16_t* custom_trace_ch1, int len_ch1,
    const int16_t* custom_trace_ch2, int len_ch2,
    const int16_t* custom_trace_ch3, int len_ch3,
    const int16_t* custom_trace_ch4, int len_ch4,
    int& out_size
) {
    int n_samples_ch1 = len_ch1 > 0 ? len_ch1 : 512;
    int n_samples_ch2 = len_ch2 > 0 ? len_ch2 : 512;
    int n_samples_ch3 = len_ch3 > 0 ? len_ch3 : 512;
    int n_samples_ch4 = len_ch4 > 0 ? len_ch4 : 512;

    int total_samples = n_samples_ch1 + n_samples_ch2 + n_samples_ch3 + n_samples_ch4;
    out_size = 256 + total_samples;

    uint16_t* evtbuf = new uint16_t[out_size]();

    // Populate basic header fields (using -2 shift)
    evtbuf[0] = out_size;                // Word 0 (EVT_LENGTH - 2 = -2? No, offset is index 0 of body)
    evtbuf[1] = 0xADC0;                  // Word 1 (Event ID / magic)
    evtbuf[3] = 256;                     // Word 3 (Header length)
    
    // GPS Date & Time (borrowing Dunhuang July 1st 2026, 09:41:41)
    evtbuf[44] = 2026;                   // Word 46 (GPS year - 2 = 44)
    evtbuf[45] = 0x0107;                 // Word 47 (GPS day, month - 2 = 45 -> Day 1, Month 7)
    evtbuf[46] = 0x2909;                 // Word 48 (GPS minute, hour - 2 = 46 -> Hour 9, Minute 41)
    evtbuf[47] = 0x0129;                 // Word 49 (Status, GPS second - 2 = 47 -> status 1, second 41)

    // Battery voltage, frequency, resolution
    evtbuf[23] = 28000;                  // Word 25 (Battery voltage - 2 = 23 -> 28V)
    evtbuf[24] = 0x0101;                 // Word 26 (Format version - 2 = 24)
    evtbuf[25] = 500;                    // Word 27 (ADC sample frequency - 2 = 25 -> 500 MHz)
    evtbuf[26] = 14;                     // Word 28 (ADC resolution - 2 = 26 -> 14-bit)
    evtbuf[28] = 0x000F;                 // Word 30 (DAQ Channel Enable - 2 = 28 -> All enabled)

    // Trace counts and configurations
    evtbuf[27] = total_samples / 16;     // Word 29 (Total number of samples / 16 - 2 = 27)
    evtbuf[28] = n_samples_ch1;          // Word 30 (Samples in channel 1 - 2 = 28)
    evtbuf[29] = n_samples_ch2;          // Word 31 (Samples in channel 2 - 2 = 29)
    evtbuf[30] = n_samples_ch3;          // Word 32 (Samples in channel 3 - 2 = 30)
    evtbuf[31] = n_samples_ch4;          // Word 33 (Samples in channel 4 - 2 = 31)
    evtbuf[32] = trigger_pattern;        // Word 34 (Trigger Pattern - 2 = 32)
    
    // Trigger windows and periods
    evtbuf[72] = t_pre_coincidence_ch2 / 2; // unit: 2ns  Word 74 (EVT_WINDOWS - 2 + 2 = 72)
    evtbuf[74] = t_pre_coincidence_ch3 / 2; // unit: 2ns Word 76 (EVT_WINDOWS - 2 + 4 = 74)
    evtbuf[110] = t_period_reg_ch2 / 10;    // unit: 10ns Word 112 (EVT_TRIGGER - 2 + 8 = 110)
    evtbuf[116] = t_period_reg_ch3 / 10;    // unit: 10ns Word 118 (EVT_TRIGGER - 2 + 14 = 116)

    // GPS location coordinates (Dunhuang: 93.9287 deg E, 40.9507 deg N, 1259.0 m alt)
    double longitude = 1.639363;         // in radians
    double latitude = 0.714725;          // in radians
    double altitude = 1259.0;            // in meters
    float temperature = 56.6f;

    std::memcpy(evtbuf + 50, &longitude, sizeof(double));
    std::memcpy(evtbuf + 54, &latitude, sizeof(double));
    std::memcpy(evtbuf + 58, &altitude, sizeof(double));
    std::memcpy(evtbuf + 62, &temperature, sizeof(float));

    // ADC traces starting offsets
    int start_idx_ch1 = 256 - 2;
    int start_idx_ch2 = start_idx_ch1 + n_samples_ch1;
    int start_idx_ch3 = start_idx_ch2 + n_samples_ch2;
    int start_idx_ch4 = start_idx_ch3 + n_samples_ch3;

    // Populate Channel 1 trace
    if (custom_trace_ch1 && len_ch1 > 0) {
        std::memcpy(evtbuf + start_idx_ch1, custom_trace_ch1, n_samples_ch1 * sizeof(uint16_t));
    }
    // Populate Channel 2 trace
    if (custom_trace_ch2 && len_ch2 > 0) {
        std::memcpy(evtbuf + start_idx_ch2, custom_trace_ch2, n_samples_ch2 * sizeof(uint16_t));
    }
    // Populate Channel 3 trace
    if (custom_trace_ch3 && len_ch3 > 0) {
        std::memcpy(evtbuf + start_idx_ch3, custom_trace_ch3, n_samples_ch3 * sizeof(uint16_t));
    }
    // Populate Channel 4 trace
    if (custom_trace_ch4 && len_ch4 > 0) {
        std::memcpy(evtbuf + start_idx_ch4, custom_trace_ch4, n_samples_ch4 * sizeof(uint16_t));
    }

    return evtbuf;
}


std::tuple<u_int16_t, uint16_t> call_scope_t2(
    uint16_t trigger_pattern,
    uint16_t t_pre_coincidence_ch2,
    uint16_t t_pre_coincidence_ch3,
    uint16_t t_period_reg_ch2,
    uint16_t t_period_reg_ch3,
    const int16_t* custom_trace_ch1, int len_ch1,
    const int16_t* custom_trace_ch2, int len_ch2,
    const int16_t* custom_trace_ch3, int len_ch3,
    const int16_t* custom_trace_ch4, int len_ch4,
    int& out_size
) {
    uint16_t* evtbuf = create_mock_event_from_scratch(
        trigger_pattern,
        t_pre_coincidence_ch2,
        t_pre_coincidence_ch3,
        t_period_reg_ch2,
        t_period_reg_ch3,
        custom_trace_ch1, len_ch1,
        custom_trace_ch2, len_ch2,
        custom_trace_ch3, len_ch3,
        custom_trace_ch4, len_ch4,
        out_size
    );

    if (!evtbuf) {
        std::cout << "WARNING: Failed to create the event buffer. Returning (65534,65534)" << std::endl;
        return std::make_tuple(65534,65534);
    }

    grand::ElecEvent ev(evtbuf, out_size);

    TemplateFLT* template_flt_x = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
    TemplateFLT* template_flt_y = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
    S_TFLite* cnn_flt = TFLT_create(2);

    std::tuple<u_int16_t, uint16_t> result = ev.scope_t2(template_flt_x, template_flt_y, cnn_flt);

    delete template_flt_x;
    delete template_flt_y;

    delete[] evtbuf;
    evtbuf = nullptr;

    return result;
}

extern "C" int call_scope_t2_c(
    uint16_t trigger_pattern,
    uint16_t t_pre_coincidence_ch2,
    uint16_t t_pre_coincidence_ch3,
    uint16_t t_period_reg_ch2,
    uint16_t t_period_reg_ch3,
    const int16_t* custom_trace_ch1, int len_ch1,
    const int16_t* custom_trace_ch2, int len_ch2,
    const int16_t* custom_trace_ch3, int len_ch3,
    const int16_t* custom_trace_ch4, int len_ch4,
    uint16_t* out0,
    uint16_t* out1,
    int* out_size
) {
    int size = 0;
    auto result = call_scope_t2(
        trigger_pattern,
        t_pre_coincidence_ch2,
        t_pre_coincidence_ch3,
        t_period_reg_ch2,
        t_period_reg_ch3,
        custom_trace_ch1, len_ch1,
        custom_trace_ch2, len_ch2,
        custom_trace_ch3, len_ch3,
        custom_trace_ch4, len_ch4,
        size
    );

    if (out0 != nullptr) {
        *out0 = std::get<0>(result);
    }
    if (out1 != nullptr) {
        *out1 = std::get<1>(result);
    }
    if (out_size != nullptr) {
        *out_size = size;
    }

    return 0;
}

#ifdef TEST_MAIN
int main() {
    int size = 0;
    
    // Create custom traces for Channels 1, 2, 3, 4
    std::vector<int16_t> custom_trace_ch1(512, 456); // fill with 456
    std::vector<int16_t> custom_trace_ch2(512, 123); // fill with 123
    std::vector<int16_t> custom_trace_ch3(512, 789); // fill with 789
    std::vector<int16_t> custom_trace_ch4(512, 111); // fill with 111
    
    uint16_t* evtbuf = create_mock_event_from_scratch(
        512,                  // trigger pattern (Channel 2 triggered)
        200,                  // t_pre_coincidence_ch2
        200,                  // t_pre_coincidence_ch3
        0x0200,               // t_period_reg_ch2 (period = 2 * 5 = 10 clock cycles)
        0x0200,               // t_period_reg_ch3
        custom_trace_ch1.data(), custom_trace_ch1.size(),
        custom_trace_ch2.data(), custom_trace_ch2.size(), 
        custom_trace_ch3.data(), custom_trace_ch3.size(),
        custom_trace_ch4.data(), custom_trace_ch4.size(),
        size
    );

    if (!evtbuf) {
        std::cerr << "Failed to create mock event from scratch." << std::endl;
        return 1;
    }

    std::cout << "Successfully created mock event buffer of size " << size << " words from scratch." << std::endl;

    // Instantiate ElecEvent
    grand::ElecEvent ev(evtbuf, size);

    // Verify trigger pattern (index 34 -> index 32 relative to m_data)
    int triggerPattern = evtbuf[34 - 2];
    std::cout << "Trigger Pattern: " << triggerPattern << std::endl;

    // Verify channel samples
    int n_samples_ch1 = evtbuf[30 - 2];
    int n_samples_ch2 = evtbuf[31 - 2];
    int n_samples_ch3 = evtbuf[32 - 2];
    int n_samples_ch4 = evtbuf[33 - 2];
    std::cout << "CH1 samples: " << n_samples_ch1 << std::endl;
    std::cout << "CH2 samples: " << n_samples_ch2 << std::endl;
    std::cout << "CH3 samples: " << n_samples_ch3 << std::endl;
    std::cout << "CH4 samples: " << n_samples_ch4 << std::endl;

    // Verify custom trace injection for CH1 (starts at 254)
    int start_idx_ch1 = (256 - 2);

    int start_idx_ch3 = (256 - 2) + n_samples_ch1 + n_samples_ch2;
    bool ch3_match = true;
    for (int i = 0; i < n_samples_ch3; ++i) {
        if (static_cast<int16_t>(evtbuf[start_idx_ch3 + i]) != 789) {
            ch3_match = false;
            break;
        }
    }
    std::cout << "CH3 trace verification: " << (ch3_match ? "SUCCESS" : "FAILED") << std::endl;
    
    int start_idx_ch2 = (256 - 2) + n_samples_ch1;
    bool ch2_match = true;
    for (int i = 0; i < n_samples_ch2; ++i) {
        if (static_cast<int16_t>(evtbuf[start_idx_ch2 + i]) != 123) {
            ch2_match = false;
            break;
        }
    }
    std::cout << "CH2 trace verification: " << (ch2_match ? "SUCCESS" : "FAILED") << std::endl;

    // Verify time decoding (getTimeNotFullDataSz because we do not include index 0 and 1 hitId/dataSizeRaw)
    auto t = ev.getTimeNotFullDataSz();
    std::cout << "Decoded Time: " << t.sec << " s, " << t.nanosec << " ns" << std::endl;
    
    // ****** NUTRIG ****** //
    TemplateFLT *template_flt_x;
    TemplateFLT *template_flt_y;
    std::cout << "Loading templates for channel X..." << std::endl;
    template_flt_x = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
    std::cout << "Done." << std::endl;
    std::cout << "Loading templates for channel Y..." << std::endl;
    template_flt_y = new TemplateFLT(TEMPLATES_XY_FILE, 500, 500, 100, 30);
    std::cout << "Done." << std::endl;
    S_TFLite *cnn_flt;
    cnn_flt = TFLT_create(2); // 2 CPU cores using multithreading of TFlite

    std::tuple<uint16_t,uint16_t> t2_res = ev.scope_t2(template_flt_x, template_flt_y, cnn_flt);
    printf("get 0: %d, get 1: %d\n", get<0>(t2_res), get<1>(t2_res));

    delete[] evtbuf;
    evtbuf = nullptr;
    return 0;
}
#endif
