/*
///////////////////////////////////
//** TEMPLATE FLT HEADER FILE ** //
///////////////////////////////////

===================TRIGGER HIERARCHY====================
- FLT = First Level Trigger
    - FLT-0 = L1 trigger on FPGA
    - FLT-1 = L2 trigger on CPU (**THIS CODE**)
- SLT = Second Level Trigger = L3 trigger on central DAQ
========================================================

This file defines the main class for the Template FLT-1.
The input for the Template FLT-1 is a trace that was triggered by the FLT-0.
The Template FLT-1 method computes the cross correlation of this input trace
with a library of air-shower signal templates at ADC level.
The correlation is computed in a small window around the maximum of the input trace.
The maximum of the input trace is determined between the "first T1 crossing" and 
"trigger time" of the FLT-0.
The template that yields the largest correlation corresponds to the best-fit template.
If this correlation value exceeds a given threshold, the input trace will be triggered
by the Template FLT-1, and passed to the SLT.
*/

#ifndef TEMPLATE_FLT_H
#define TEMPLATE_FLT_H

#include <iostream>
#include <vector>
#include <string>
#include <tuple>
#include <eigen3/Eigen/Dense>

class TemplateFLT{
    private:
        /*
        ------------------
        PRIVATE ATTRIBUTES
        ------------------
        */

        // ADC sampling rate [MHz]
        int adc_sampling_rate;
        // Simulation sampling rate [MHz]
        int sim_sampling_rate;
        // Desampling factor = simulation sampling rate / ADC sampling rate
        int desampling_factor;

        // Number of samples of an original template (simulation sampling rate)
        int size_template;
        // Number of samples of a desampled template (ADC sampling rate)
        int size_template_desampled;
        // Sample of peak position of an original template (simluation sampling rate)
        int sample_peak_template;
        // Sample of peak position of a desampled template (ADC sampling rate)
        int sample_peak_template_desampled;

        // Window around trace maximum for which to compute cross correlation
        Eigen::Array2i corr_window;
        // Threshold for the correlation value in order to trigger
        float corr_thresh;

        /*
        ---------------
        PRIVATE METHODS
        ---------------
        */

        std::tuple<int,float> compute_max_correlation(const Eigen::ArrayXi& trace,
                                                      const Eigen::ArrayXf& templ,
                                                      const bool& norm=true);

    public:
        /*
        -----------------
        PUBLIC ATTRIBUTES
        -----------------
        */

        // Templates loaded at `sim_sampling_rate`
        std::vector< Eigen::ArrayXf > templates;
        // Templates desampled to `adc_sampling_rate`
        std::vector< std::vector< Eigen::ArrayXf > > templates_desampled;

        // ID of best-fit template
        int template_id_best;
        // Index of the best desampling of the best-fit template
        int idx_template_desampled_best;
        // Best-fit time of the pulse peak
        int t_peak_best;
        // Maximum correlation yielding the best-fit template
        float corr_max_best;

        /*
        ------------
        CONSTRUCTORS
        ------------
        */

        TemplateFLT();

        TemplateFLT(const std::string& template_file_name,
                    const int& adc_sampling_rate = 500,
                    const int& sim_sampling_rate = 2000,
                    const int& size_template = 400,
                    const int& sample_peak_template = 120,
                    const Eigen::Array2i& corr_window = {-10,10});

        /*
        -------
        SETTERS
        -------
        */

        void set_sampling_rates(const int& adc_sampling_rate,
                                const int& sim_sampling_rate);
        void set_corr_window(const int& start,
                             const int& end);
        void set_corr_thresh(const float& corr_thresh);

        /*
        -------
        GETTERS
        -------
        */

        int get_adc_sampling_rate();
        int get_sim_sampling_rate();
        int get_desampling_factor();
        int get_size_template();
        int get_size_template_desampled();
        int get_sample_peak_template();
        int get_sample_peak_template_desampled();
        Eigen::Array2i get_corr_window();
        float get_corr_thresh();

        /*
        --------------
        PUBLIC METHODS
        --------------
        */

        void load_templates(const std::string& template_file_name,
                            const int& size_template = 400,
                            const int& sample_peak_template = 120);
        void desample_templates();
        void template_fit(const Eigen::ArrayXi& trace,
                          const int& t_max);
        bool trigger(const Eigen::ArrayXi& trace);
};
# endif // TEMPLATE_FLT_H