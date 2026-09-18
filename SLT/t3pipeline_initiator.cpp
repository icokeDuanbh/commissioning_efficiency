#include "../grand_daq/grand-daq/include/trigger_pipeline.h"

#include <iostream>
#include <string>

// =============================================================================
// Shared-library API (extern "C" for ctypes)
//
// Designed for the single-event-per-call use case:
//   handle = slt_pipeline_create(...)
//   ret    = slt_pipeline_run(handle, hits, ..., output_arrays)
//              returns  1 : triggered (T3Filter passed)
//                       0 : not triggered (nhit not met, or T3Filter rejected)
//                      -1 : bad arguments
//   slt_pipeline_destroy(handle)
//
// The duplicate-filter history is preserved across calls, matching real DAQ.
// =============================================================================

extern "C" void* slt_pipeline_create(
    int         trigger_type,
    int         nhit_threshold,
    uint64_t    time_window_ns,
    int         duplicate_filter_enable,     // 0 or 1
    int         duplicate_time_diff_ns,
    int         duplicate_min_pair,
    int         duplicate_history_size,
    int         causal_window_enabled,       // 0 or 1
    double      causal_tolerance_ns,
    const char* antenna_distances_file,      // "" or nullptr to disable
    int         template_match_filter_enable, // 0 or 1
    double      template_match_coefficient
) {
    grand::TriggerPipelineConfig cfg;
    cfg.trigger_type                  = trigger_type;
    cfg.nhit_threshold                = nhit_threshold;
    cfg.time_window_ns                = time_window_ns;
    cfg.duplicate_filter_enable       = (duplicate_filter_enable != 0);
    cfg.duplicate_time_diff_ns        = duplicate_time_diff_ns;
    cfg.duplicate_min_pair            = duplicate_min_pair;
    cfg.duplicate_history_size        = duplicate_history_size;
    cfg.causal_window_enabled         = (causal_window_enabled != 0);
    cfg.causal_tolerance_ns           = causal_tolerance_ns;
    cfg.template_match_filter_enable  = (template_match_filter_enable != 0);
    cfg.template_match_coefficient    = template_match_coefficient;
    if (antenna_distances_file && antenna_distances_file[0] != '\0') {
        cfg.antenna_distances_file = antenna_distances_file;
    }
    try {
        return new grand::TriggerPipeline(cfg);
    } catch (const std::exception& e) {
        std::cerr << "slt_pipeline_create: " << e.what() << std::endl;
        return nullptr;
    }
}

extern "C" void slt_pipeline_destroy(void* handle) {
    delete static_cast<grand::TriggerPipeline*>(handle);
}

// Seed the duplicate-filter history with one event's hits.
//
// Computes the pair-time-difference fingerprint from the flat
// (du_ids[], timestamps_ns[], n_hits) arrays and pushes it directly
// into T3Filter::history_, bypassing nhit / causal / template-match.
//
//   handle          — opaque pointer from slt_pipeline_create()
//   du_ids          — DU identifiers, length n_hits
//   timestamps_ns   — per-DU nanosecond timestamps (intra-second, i.e.
//                     the raw ns field from the DuplicateRejectLog), length n_hits
//   n_hits          — number of DU hits in this event (must be >= 2)
//
// Returns 0 on success, -1 on bad arguments (null handle/arrays, n_hits < 2).
extern "C" int slt_pipeline_seed_history(
    void*           handle,
    const uint32_t* du_ids,
    const uint64_t* timestamps_ns,
    int             n_hits
) {
    if (!handle || !du_ids || !timestamps_ns || n_hits < 2) {
        return -1;
    }
    auto* pipeline = static_cast<grand::TriggerPipeline*>(handle);
    return pipeline->seedDuplicateHistory(du_ids, timestamps_ns, n_hits) ? 0 : -1;
}


// Run the trigger on one bucket of hits (at most one event expected).
//
// Input hits — parallel flat arrays, all length n_hits:
//   du_ids[]          uint32  DU identifier
//   timestamps_ns[]   uint64  T2 timestamp in nanoseconds
//   related_values[]  uint16  template-match score from FLT1 (fixed-point / 65536);
//                             must not be null — used by the template-match filter
//
// Output arrays must be pre-allocated by the caller to at least max_items elements:
//   out_du_count       scalar  number of post-filter DUs written
//   out_du_ids         uint32  post-filter DU IDs          [max_items]
//   out_trigger_ts     uint64  post-filter timestamps (ns) [max_items]
//   out_window_du_count scalar number of pre-filter window DU IDs written
//   out_window_du_ids  uint32  pre-filter window DU IDs    [max_items]
//   out_window_ts_count scalar number of pre-filter window timestamps written
//   out_window_ts      uint64  pre-filter window timestamps [max_items]
//
// Returns:
//   1  triggered (nhit met + T3Filter passed)
//   0  not triggered
//  -1  bad arguments (null handle or hits)
extern "C" int slt_pipeline_run(
    void*           handle,
    const uint32_t* du_ids,
    const uint64_t* timestamps_ns,
    const uint16_t* related_values,     // nullable
    int             n_hits,
    uint64_t        tm_id,
    uint64_t        bucket_timestamp_ns,
    int             max_items,          // capacity of every output array
    // Note: related_values is required; pass an array of zeros if FLT1 scores
    // are unavailable and template_match_filter_enable=0.
    int*            out_du_count,
    uint32_t*       out_du_ids,
    uint64_t*       out_trigger_ts,
    int*            out_window_du_count,
    uint32_t*       out_window_du_ids,
    int*            out_window_ts_count,
    uint64_t*       out_window_ts
) {
    if (!handle || !du_ids || !timestamps_ns || !related_values || n_hits <= 0) {
        return -1;
    }

    auto* pipeline = static_cast<grand::TriggerPipeline*>(handle);

    std::vector<grand::TriggerPipeline::TestHit> hits(static_cast<std::size_t>(n_hits));
    for (int i = 0; i < n_hits; ++i) {
        hits[i].du_id           = du_ids[i];
        hits[i].timestamp_ns    = timestamps_ns[i];
        hits[i].trigger_channel = 0;
        hits[i].related_value   = related_values[i];
    }

    grand::TriggerResults results =
        pipeline->testOnly_collectTriggerResults(hits, tm_id, bucket_timestamp_ns);

    if (results.empty()) {
        return 0;   // not triggered
    }

    // We only care about the first result (single-event assumption).
    const grand::TriggerResult& r = results[0];

    // Post-filter DUs
    int dc = static_cast<int>(r.du_ids.size());
    if (dc > max_items) dc = max_items;
    if (out_du_count) *out_du_count = dc;
    if (out_du_ids) {
        for (int j = 0; j < dc; ++j) out_du_ids[j] = r.du_ids[j];
    }
    int tc = static_cast<int>(r.trigger_timestamps.size());
    if (tc > max_items) tc = max_items;
    if (out_trigger_ts) {
        for (int j = 0; j < tc; ++j) out_trigger_ts[j] = r.trigger_timestamps[j];
    }

    // Pre-filter window
    int wdc = static_cast<int>(r.window_du_ids.size());
    if (wdc > max_items) wdc = max_items;
    if (out_window_du_count) *out_window_du_count = wdc;
    if (out_window_du_ids) {
        for (int j = 0; j < wdc; ++j) out_window_du_ids[j] = r.window_du_ids[j];
    }
    int wtc = static_cast<int>(r.window_timestamps.size());
    if (wtc > max_items) wtc = max_items;
    if (out_window_ts_count) *out_window_ts_count = wtc;
    if (out_window_ts) {
        for (int j = 0; j < wtc; ++j) out_window_ts[j] = r.window_timestamps[j];
    }

    return 1;   // triggered
}

// =============================================================================
// Optional standalone test  (compile with -DTEST_MAIN)
// =============================================================================
#ifdef TEST_MAIN
#include <cstdio>
int main() {
    void* h = slt_pipeline_create(
        2, 5, 15000, 1, 100, 2, 10, 1, 100.0,
        "/Users/xishui/Dropbox/Project/GRAND/Event_injector/"
        "grand_daq/grand-daq/cfgs/detector_distances_May24_v2.txt",
        0, 0.0
    );

    uint32_t du[]  = {103, 109, 1010, 1011, 1012};
    uint64_t ts[]  = {1000000000ULL, 1000003000ULL, 1000006000ULL,
                      1000009000ULL, 1000012000ULL};
    constexpr int N = 64;
    int dc = 0, wdc = 0, wtc = 0;
    uint32_t du_out[N] = {}, wdu_out[N] = {};
    uint64_t ts_out[N] = {}, wts_out[N] = {};

    uint16_t rv[] = {0, 0, 0, 0, 0};  // no template-match filter in this test
    int ret = slt_pipeline_run(h, du, ts, rv, 5, 1000ULL, 1000000000ULL,
                               N, &dc, du_out, ts_out, &wdc, wdu_out, &wtc, wts_out);
    std::printf("Run 1: ret=%d  du_count=%d\n", ret, dc);

    ret = slt_pipeline_run(h, du, ts, rv, 5, 1001ULL, 2000000000ULL,
                           N, &dc, du_out, ts_out, &wdc, wdu_out, &wtc, wts_out);
    std::printf("Run 2 (duplicate): ret=%d  du_count=%d\n", ret, dc);

    slt_pipeline_destroy(h);
}
#endif
