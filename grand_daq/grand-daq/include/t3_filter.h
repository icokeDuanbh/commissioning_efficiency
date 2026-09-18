#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace grand {

// DU T2 related_value 为定点数，除以 65536 得到与 template_match_coefficient 同量纲的分数。
inline constexpr double kRelatedValueScale = 65536.0;

// 单次 hit：id 为 DU，time 为 T2 时间戳 (ns)，与 TriggerPipeline 里 Hit 同信息不同整型
struct Detector {
    int id = 0;
    int time = 0;
    uint16_t related_value = 0;
};

using PairKey = std::pair<int, int>;
using PairDiffs = std::map<PairKey, int>; // 有序 DU 对 → 时间差 (ns)
using AntennaDistanceMap = std::unordered_map<uint32_t, std::unordered_map<uint32_t, double>>;

struct T3FilterConfig {
    bool duplicate_filter_enable = true;
    int duplicate_time_diff_ns = 100;
    std::size_t duplicate_history_size = 10;
    int duplicate_min_pair = 2;

    bool causal_filter_enable = false;
    double causal_tolerance_ns = 100.0;
    std::string antenna_distances_file;

    int nhit_threshold = 5; // 因果子集至少多大才保留（与 pipeline 配置一致）

    bool template_match_filter_enable = false;
    double template_match_coefficient = 0.0;
};

enum class T3FilterRejectReason : uint8_t {
    None,
    Duplicate,
    CausalSubsetTooSmall,
    TemplateMatchBelowThreshold,
};

struct T3FilterDecision {
    std::vector<size_t> selected_indices;
    T3FilterRejectReason reject_reason = T3FilterRejectReason::None;
    std::vector<size_t> duplicate_log_indices;
    bool causal_evaluated = false;
    std::size_t causal_input_count = 0;
    std::size_t causal_unique_du_count = 0;
    std::size_t causal_selected_count = 0;
    uint64_t causal_elapsed_ns = 0;
    bool causal_bypassed_high_multiplicity = false;
    bool template_match_evaluated = false;
    double template_match_avg_related_value = 0.0;
    std::size_t template_match_detector_count = 0;

    bool duplicateRejected() const {
        return selected_indices.empty() && reject_reason == T3FilterRejectReason::Duplicate;
    }
};

// T3 层重复/因果过滤：维护 pair-diff 历史，可选加载天线几何得到光速下界
class T3Filter {
public:
    explicit T3Filter(const T3FilterConfig& config) : config_(config) {
        if (config_.causal_filter_enable && !config_.antenna_distances_file.empty()) {
            loadAntennaDistances(config_.antenna_distances_file);
        }
    }

    // 文本行：du_a du_b distance_m；内部换算为光程 ns 填双向表
    bool loadAntennaDistances(const std::string& filepath) {
        std::ifstream infile(filepath);
        if (!infile) {
            antenna_distances_loaded_ = false;
            return false;
        }

        antenna_distance_map_.clear();
        std::string line;
        while (std::getline(infile, line)) {
            std::istringstream iss(line);
            uint32_t du_a = 0;
            uint32_t du_b = 0;
            double distance_m = 0.0;
            if (!(iss >> du_a >> du_b >> distance_m)) {
                continue;
            }
            const double time_ns = distance_m / 299702547.0 * 1e9;
            antenna_distance_map_[du_a][du_b] = time_ns;
            antenna_distance_map_[du_b][du_a] = time_ns;
        }
        antenna_distances_loaded_ = !antenna_distance_map_.empty();
        return antenna_distances_loaded_;
    }

    // 所有无序 DU 对的时间差，键为 minmax(id_i,id_j)
    static PairDiffs calculatePairDifferences(const std::vector<Detector>& detectors) {
        PairDiffs pairs;
        for (std::size_t i = 0; i < detectors.size(); ++i) {
            for (std::size_t j = i + 1; j < detectors.size(); ++j) {
                const PairKey key = std::minmax(detectors[i].id, detectors[j].id);
                pairs[key] = detectors[i].time - detectors[j].time;
            }
        }
        return pairs;
    }

    // 与历史 pair-diff 比对，足够多对落在 duplicate_time_diff_ns 内则判重复
    bool isDuplicateEvent(const std::vector<Detector>& detectors) {
        if (!config_.duplicate_filter_enable) {
            return false;
        }

        const PairDiffs current_pairs = calculatePairDifferences(detectors);
        return isDuplicateEventWithPairs(current_pairs);
    }

    // 因果开启且表已加载：返回因果相容子集对应的 DU；否则退化为全体 DU 去重
    std::vector<uint16_t> causalWindowFilter(const std::vector<Detector>& detectors) {
        if (!config_.causal_filter_enable || !antenna_distances_loaded_) {
            std::unordered_set<uint16_t> du_set;
            for (const auto& detector : detectors) {
                du_set.insert(static_cast<uint16_t>(detector.id));
            }
            return std::vector<uint16_t>(du_set.begin(), du_set.end());
        }

        std::set<uint16_t> du_set;
        std::unordered_set<int> unique_dus;
        unique_dus.reserve(detectors.size());
        for (const auto& detector : detectors) {
            unique_dus.insert(detector.id);
        }

        std::vector<size_t> selected_indices;
        if (unique_dus.size() >= kHighMultiplicityBypassUniqueDuThreshold) {
            selected_indices.resize(detectors.size());
            std::iota(selected_indices.begin(), selected_indices.end(), 0);
        } else {
            selected_indices = causalWindowFilterIndices(detectors);
        }
        for (std::size_t idx : selected_indices) {
            du_set.insert(static_cast<uint16_t>(detectors[idx].id));
        }
        return std::vector<uint16_t>(du_set.begin(), du_set.end());
    }

    // 返回保留的 detector 下标和剔除原因；重复或因果不足则 selected_indices 为空
    T3FilterDecision filterDecision(const std::vector<Detector>& detectors) {
        std::vector<size_t> all_indices(detectors.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);

        if (config_.causal_filter_enable && antenna_distances_loaded_) {
            T3FilterDecision decision;
            decision.causal_evaluated = true;
            decision.causal_input_count = detectors.size();
            std::unordered_set<int> unique_dus;
            unique_dus.reserve(detectors.size());
            for (const auto& detector : detectors) {
                unique_dus.insert(detector.id);
            }
            decision.causal_unique_du_count = unique_dus.size();

            std::vector<size_t> selected;
            if (decision.causal_unique_du_count >= kHighMultiplicityBypassUniqueDuThreshold) {
                selected = all_indices;
                decision.causal_bypassed_high_multiplicity = true;
            } else {
                const auto start = std::chrono::steady_clock::now();
                selected = findMaximumCausalSubsetIndices(detectors);
                const auto end = std::chrono::steady_clock::now();
                decision.causal_elapsed_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                if (decision.causal_elapsed_ns >= kSlowMaximumCliqueLogThresholdNs) {
                    std::cerr << "SLOW_MAX_CLIQUE detectors=" << detectors.size()
                              << " unique_du=" << decision.causal_unique_du_count
                              << " selected=" << selected.size()
                              << " elapsed_ms=" << (decision.causal_elapsed_ns / 1000000.0)
                              << std::endl;
                }
            }
            decision.causal_selected_count = selected.size();

            if (selected.size() < static_cast<std::size_t>(config_.nhit_threshold)) {
                decision.reject_reason = T3FilterRejectReason::CausalSubsetTooSmall;
                return decision;
            }

            if (config_.template_match_filter_enable) {
                decision.template_match_evaluated = true;
                decision.template_match_detector_count = selected.size();
                decision.template_match_avg_related_value =
                    averageRelatedValue(detectors, selected);
                if (!passesTemplateMatchAvg(decision.template_match_avg_related_value, selected)) {
                    decision.reject_reason = T3FilterRejectReason::TemplateMatchBelowThreshold;
                    return decision;
                }
            }

            if (config_.duplicate_filter_enable) {
                const auto selected_detectors = selectDetectors(detectors, selected);
                const PairDiffs selected_pairs = calculatePairDifferences(selected_detectors);
                if (isDuplicateEventWithPairs(selected_pairs)) {
                    decision.reject_reason = T3FilterRejectReason::Duplicate;
                    decision.duplicate_log_indices = std::move(selected);
                    return decision;
                }
            }

            decision.selected_indices = std::move(selected);
            return decision;
        }

        T3FilterDecision decision;
        if (config_.template_match_filter_enable) {
            decision.template_match_evaluated = true;
            decision.template_match_detector_count = all_indices.size();
            decision.template_match_avg_related_value =
                averageRelatedValue(detectors, all_indices);
            if (!passesTemplateMatchAvg(decision.template_match_avg_related_value, all_indices)) {
                decision.reject_reason = T3FilterRejectReason::TemplateMatchBelowThreshold;
                return decision;
            }
        }
        if (isDuplicateEvent(detectors)) {
            decision.reject_reason = T3FilterRejectReason::Duplicate;
            decision.duplicate_log_indices = std::move(all_indices);
            return decision;
        }
        decision.selected_indices = std::move(all_indices);
        return decision;
    }

    // 返回保留的 detector 下标；保留旧接口，避免扩大调用面改动
    std::vector<size_t> filterIndices(const std::vector<Detector>& detectors) {
        return filterDecision(detectors).selected_indices;
    }

    // 与 filterIndices 相同语义，输出 DU id 列表
    std::vector<uint16_t> filter(const std::vector<Detector>& detectors) {
        const auto selected = filterIndices(detectors);
        if (selected.empty()) {
            return {};
        }
        std::set<uint16_t> du_set;
        for (std::size_t idx : selected) {
            du_set.insert(static_cast<uint16_t>(detectors[idx].id));
        }
        return std::vector<uint16_t>(du_set.begin(), du_set.end());
    }

    // 清空重复检测滑动状态
    void reset() {
        prev_pairs_.clear();
        history_.clear();
    }

    // Directly seed the duplicate-filter history with a pre-computed PTD set.
    // Bypasses all other cuts (nhit, causal, template-match).
    // Used by the event injector to warm up the history deque from a
    // DuplicateRejectLog before the first injected event is submitted.
    void seedHistory(const PairDiffs& pairs) {
        if (pairs.empty()) return;
        history_.push_back(pairs);
        if (history_.size() > config_.duplicate_history_size) {
            history_.pop_front();
        }
    }

private:
    T3FilterConfig config_;
    PairDiffs prev_pairs_;
    std::deque<PairDiffs> history_;
    AntennaDistanceMap antenna_distance_map_;
    bool antenna_distances_loaded_ = false;
    static constexpr std::size_t kHighMultiplicityBypassUniqueDuThreshold = 20;
    static constexpr uint64_t kSlowMaximumCliqueLogThresholdNs = 100000000ULL;

    static std::vector<Detector> selectDetectors(const std::vector<Detector>& detectors,
                                                 const std::vector<size_t>& indices) {
        std::vector<Detector> selected;
        selected.reserve(indices.size());
        for (std::size_t idx : indices) {
            if (idx < detectors.size()) {
                selected.push_back(detectors[idx]);
            }
        }
        return selected;
    }

    static double averageRelatedValue(const std::vector<Detector>& detectors,
                                      const std::vector<size_t>& indices) {
        if (indices.empty()) {
            return 0.0;
        }
        uint64_t sum = 0;
        for (std::size_t idx : indices) {
            if (idx < detectors.size()) {
                sum += detectors[idx].related_value;
            }
        }
        return static_cast<double>(sum) / static_cast<double>(indices.size()) / kRelatedValueScale;
    }

    bool passesTemplateMatchAvg(double avg_related_value,
                                const std::vector<size_t>& indices) const {
        if (indices.empty()) {
            return false;
        }
        return avg_related_value > config_.template_match_coefficient;
    }

    bool passesTemplateMatch(const std::vector<Detector>& detectors,
                             const std::vector<size_t>& indices) const {
        return passesTemplateMatchAvg(averageRelatedValue(detectors, indices), indices);
    }

    bool checkDuplicate(const PairDiffs& current_pairs) const {
        auto match_count = [&](const PairDiffs& base) {
            int similar = 0;
            for (const auto& kv : current_pairs) {
                const auto it = base.find(kv.first);
                if (it == base.end()) {
                    continue;
                }
                const int diff = kv.second - it->second;
                const int abs_diff = diff >= 0 ? diff : -diff;
                if (abs_diff < config_.duplicate_time_diff_ns) {
                    ++similar;
                    if (similar >= config_.duplicate_min_pair) {
                        return true;
                    }
                }
            }
            return false;
        };

        for (const auto& hist : history_) {
            if (match_count(hist)) {
                return true;
            }
        }
        if (!prev_pairs_.empty() && match_count(prev_pairs_)) {
            return true;
        }
        return false;
    }

    bool isDuplicateEventWithPairs(const PairDiffs& current_pairs) {
        const bool duplicate = checkDuplicate(current_pairs);
        if (duplicate) {
            history_.push_back(current_pairs);
            if (history_.size() > config_.duplicate_history_size) {
                history_.pop_front();
            }
        } else {
            prev_pairs_ = current_pairs;
        }
        return duplicate;
    }

    bool areDetectorsCausallyCompatible(const Detector& lhs, const Detector& rhs) const {
        if (lhs.id == rhs.id) {
            return false;
        }
        double t_geom_ns = 0.0;
        bool has_distance = false;
        auto lhs_it = antenna_distance_map_.find(static_cast<uint32_t>(lhs.id));
        if (lhs_it != antenna_distance_map_.end()) {
            auto dist_it = lhs_it->second.find(static_cast<uint32_t>(rhs.id));
            if (dist_it != lhs_it->second.end()) {
                has_distance = true;
                t_geom_ns = dist_it->second;
            }
        }
        if (!has_distance) {
            auto rhs_it = antenna_distance_map_.find(static_cast<uint32_t>(rhs.id));
            if (rhs_it != antenna_distance_map_.end()) {
                auto dist_it = rhs_it->second.find(static_cast<uint32_t>(lhs.id));
                if (dist_it != rhs_it->second.end()) {
                    has_distance = true;
                    t_geom_ns = dist_it->second;
                }
            }
        }
        if (!has_distance) {
            return false;
        }
        const int dt = lhs.time - rhs.time;
        const double abs_dt = dt >= 0 ? static_cast<double>(dt) : static_cast<double>(-dt);
        return abs_dt <= (t_geom_ns + config_.causal_tolerance_ns);
    }

    std::vector<size_t> findMaximumCausalSubsetIndices(const std::vector<Detector>& detectors) const {
        const std::size_t n = detectors.size();
        if (n == 0) {
            return {};
        }

        std::vector<std::vector<size_t>> neighbors(n);
        std::vector<size_t> degrees(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                if (!areDetectorsCausallyCompatible(detectors[i], detectors[j])) {
                    continue;
                }
                neighbors[i].push_back(j);
                neighbors[j].push_back(i);
                ++degrees[i];
                ++degrees[j];
            }
        }
        for (auto& adjacent : neighbors) {
            std::sort(adjacent.begin(), adjacent.end());
        }

        std::vector<size_t> candidates(n);
        std::iota(candidates.begin(), candidates.end(), 0);
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](std::size_t lhs, std::size_t rhs) {
                             if (degrees[lhs] != degrees[rhs]) {
                                 return degrees[lhs] > degrees[rhs];
                             }
                             if (detectors[lhs].time != detectors[rhs].time) {
                                 return detectors[lhs].time < detectors[rhs].time;
                             }
                             if (detectors[lhs].id != detectors[rhs].id) {
                                 return detectors[lhs].id < detectors[rhs].id;
                             }
                             return lhs < rhs;
                         });

        const auto intersects_with_neighbors =
            [&](const std::vector<size_t>& nodes, std::size_t pivot) {
                std::vector<size_t> out;
                out.reserve(nodes.size());
                const auto& pivot_neighbors = neighbors[pivot];
                for (std::size_t node : nodes) {
                    if (std::binary_search(pivot_neighbors.begin(), pivot_neighbors.end(), node)) {
                        out.push_back(node);
                    }
                }
                return out;
            };

        const auto count_neighbors_in_set =
            [&](std::size_t pivot, const std::vector<size_t>& nodes) {
                std::size_t count = 0;
                const auto& pivot_neighbors = neighbors[pivot];
                for (std::size_t node : nodes) {
                    if (std::binary_search(pivot_neighbors.begin(), pivot_neighbors.end(), node)) {
                        ++count;
                    }
                }
                return count;
            };

        std::vector<size_t> best;
        std::vector<size_t> current;
        std::function<void(std::vector<size_t>, std::vector<size_t>)> bron_kerbosch =
            [&](std::vector<size_t> p, std::vector<size_t> x) {
                if (current.size() + p.size() <= best.size()) {
                    return;
                }
                if (p.empty() && x.empty()) {
                    std::vector<size_t> candidate = current;
                    std::sort(candidate.begin(), candidate.end());
                    if (candidate.size() > best.size() ||
                        (candidate.size() == best.size() && candidate < best)) {
                        best = std::move(candidate);
                    }
                    return;
                }

                std::size_t pivot = std::numeric_limits<std::size_t>::max();
                std::size_t pivot_score = 0;
                std::vector<size_t> union_px = p;
                union_px.insert(union_px.end(), x.begin(), x.end());
                for (std::size_t node : union_px) {
                    const std::size_t score = count_neighbors_in_set(node, p);
                    if (pivot == std::numeric_limits<std::size_t>::max() || score > pivot_score) {
                        pivot = node;
                        pivot_score = score;
                    }
                }

                std::vector<size_t> branch_nodes;
                branch_nodes.reserve(p.size());
                for (std::size_t node : p) {
                    if (pivot == std::numeric_limits<std::size_t>::max() ||
                        !std::binary_search(neighbors[pivot].begin(), neighbors[pivot].end(), node)) {
                        branch_nodes.push_back(node);
                    }
                }
                if (branch_nodes.empty()) {
                    branch_nodes = p;
                }

                for (std::size_t node : branch_nodes) {
                    auto it = std::find(p.begin(), p.end(), node);
                    if (it == p.end()) {
                        continue;
                    }
                    current.push_back(node);
                    bron_kerbosch(intersects_with_neighbors(p, node),
                                  intersects_with_neighbors(x, node));
                    current.pop_back();

                    p.erase(std::remove(p.begin(), p.end(), node), p.end());
                    x.push_back(node);
                    if (current.size() + p.size() <= best.size()) {
                        return;
                    }
                }
            };

        bron_kerbosch(candidates, {});
        return best;
    }

    std::vector<size_t> causalWindowFilterIndices(const std::vector<Detector>& detectors) {
        std::vector<size_t> all_indices(detectors.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);
        if (!config_.causal_filter_enable || !antenna_distances_loaded_) {
            return all_indices;
        }

        std::vector<size_t> best = findMaximumCausalSubsetIndices(detectors);
        if (best.size() < static_cast<std::size_t>(config_.nhit_threshold)) {
            return {};
        }
        return best;
    }
};

} // namespace grand
