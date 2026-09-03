#include <iostream>
#include "template_FLT.h"
#include "utils.h"
#include <chrono>

using namespace std;

int N_ITER = 20000;
string TEST_TRACE_FILE = "test_trace.txt";
string TEMPLATES_XY_FILE = "templates_96_XY_rfv2.txt";

int main() {
    // Load test trace
    vector<Eigen::ArrayXi> test_trace = load_test_trace(TEST_TRACE_FILE);

    // Load TemplateFLT objects for X and Y polarizations
    TemplateFLT flt_x(TEMPLATES_XY_FILE), flt_y(TEMPLATES_XY_FILE);

    // Loop over all desired iterations
    // You can time the `main` executable in your preferred shell
    int t_max_x, t_max_y, trace_max_x, trace_max_y;
    for (int i=0; i<N_ITER; i++){
        // Evaluate X
        trace_max_x = test_trace[0].maxCoeff(&t_max_x);
        flt_x.template_fit(test_trace[0],t_max_x);

        // Evaluate Y
        trace_max_y = test_trace[1].maxCoeff(&t_max_y);
        flt_y.template_fit(test_trace[1],t_max_y);
    }

    // Print the last evaluation of the template FLT
    cout<<"*** POLARIZATION X ***"<<"\n";
    cout<<"t_peak_best = "<<flt_x.t_peak_best<<"\n";
    cout<<"corr_max_best = "<<flt_x.corr_max_best<<"\n";
    cout<<"template_id_best = "<<flt_x.template_id_best<<"\n";
    cout<<"idx_template_desampled_best = "<<flt_x.idx_template_desampled_best<<endl<<endl;

    cout<<"*** POLARIZATION Y ***"<<"\n";
    cout<<"t_peak_best = "<<flt_y.t_peak_best<<"\n";
    cout<<"corr_max_best = "<<flt_y.corr_max_best<<"\n";
    cout<<"template_id_best = "<<flt_y.template_id_best<<"\n";
    cout<<"idx_template_desampled_best = "<<flt_y.idx_template_desampled_best<<endl;

    return 0;
};