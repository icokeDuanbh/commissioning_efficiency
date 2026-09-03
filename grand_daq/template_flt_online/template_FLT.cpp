///////////////////////////////////
//** TEMPLATE FLT SOURCE FILE ** //
///////////////////////////////////

#include <fstream>
#include "template_FLT.h"
#include "error_handling.h"
#include "utils.h"

using namespace std;

/*
------------
CONSTRUCTORS
------------
*/

/*
Constructor that creates an "empty" TemplateFLT object.
*/
TemplateFLT::TemplateFLT(){
   this->adc_sampling_rate = 0;
   this->sim_sampling_rate = 0;
   this->desampling_factor = 0;
   this->size_template = 0;
   this->size_template_desampled = 0;
   this->sample_peak_template = 0;
   this->sample_peak_template_desampled = 0;
   this->corr_window = {0,0};
}


/*
Constructor that loads in templates stored in a txt file.
Also desampled the templates.

Arguments
---------
`template_file_name` : Path to the txt file storing the templates.

`size_template` : Number of samples of the template. Default is 400.

`adc_sampling_rate` [MHz] : Sampling rate of the ADC. Default is 500 MHz.

`sim_sampling_rate` [MHz] : Sampling rate of the simulations that yield the templates. Default is 2000 MHz.

`corr_window` : Correlation window `{start,end}` relative to the trace maximum. Default is {-10,10}. 
                `start` corresponds to the number of samples BEFORE trace maximum. Must be <= 0.
                `end` corresponds to the number of samples AFTER trace maximum. Must be >= 0.
*/
TemplateFLT::TemplateFLT(const string& template_file_name,
                         const int& adc_sampling_rate,
                         const int& sim_sampling_rate,
                         const int& size_template,
                         const int& sample_peak_template,
                         const Eigen::Array2i& corr_window){

    this->adc_sampling_rate = adc_sampling_rate;
    this->sim_sampling_rate = sim_sampling_rate;
    this->desampling_factor = sim_sampling_rate/adc_sampling_rate;
    this->corr_window = corr_window;

    load_templates(template_file_name,size_template,sample_peak_template);
}


/*
-------
SETTERS
-------
*/

/*
Setter for `adc_sampling_rate` and `sim_sampling_rate`.

Arguments
---------
`adc_sampling_rate` : ADC sampling rate [MHz].

`sim_sampling_rate` : Simulation sampling rate [MHz].
*/
void TemplateFLT::set_sampling_rates(const int& adc_sampling_rate,
                                     const int& sim_sampling_rate){
    // Check that the simulation sampling rate is at least as large as ADC sampling rate
    if (sim_sampling_rate < adc_sampling_rate){
        string err_msg = "Simulation sampling rate must be at least as large as ADC sampling rate!";
        throwError(err_msg,__FILE__,__LINE__);
    }

    // Set the values
    this->adc_sampling_rate = adc_sampling_rate;
    this->sim_sampling_rate = sim_sampling_rate;
}



/*
Setter for `corr_window`.
Window must be relative to the position of trace maximum.

Arguments
---------
`start` : Number of samples BEFORE trace maximum. Must be < 0.

`end` : Number of samples AFTER trace maximum. Must be > 0.
*/
void TemplateFLT::set_corr_window(const int& start,
                                  const int& end){
    // Check if corr window conditions are satisfied
    if(start > 0){
        string err_msg = "start must be < 0! Number of samples BEFORE trace maximum.";
        throwError(err_msg,__FILE__,__LINE__);
    }
    if(end < 0){
        string err_msg = "end must be > 0! Number of samples AFTER trace maximum.";
        throwError(err_msg,__FILE__,__LINE__);
    }

    // Set the values
    this->corr_window = {start,end};

    return;
}


/*
Setter for `corr_thresh`.

Arguments
---------
`corr_thresh` : Correlation threshold. Must be between [0,1].
*/
void TemplateFLT::set_corr_thresh(const float& corr_thresh){
    // Check that threshold is within [0,1]
    if (corr_thresh < 0 || corr_thresh > 1){
        string err_msg = "Correlation threshold must be between [0,1]!";
        throwError(err_msg,__FILE__,__LINE__);
    }

    // Set the value
    this->corr_thresh = corr_thresh;

    return;
}


/*
-------
GETTERS
-------
*/

/*
Getter for `adc_sampling_rate`.
*/
int TemplateFLT::get_adc_sampling_rate(){
    return this->adc_sampling_rate;
}

/*
Getter for `sim_sampling_rate`.
*/
int TemplateFLT::get_sim_sampling_rate(){
    return this->sim_sampling_rate;
}

/*
Getter for `desampling_factor`.
*/
int TemplateFLT::get_desampling_factor(){
    return this->desampling_factor;
}

/*
Getter for `size_template`.
*/
int TemplateFLT::get_size_template(){
    return this->size_template;
}

/*
Getter for `size_template_desampled`.
*/
int TemplateFLT::get_size_template_desampled(){
    return this->size_template_desampled;
}

/*
Getter for `sample_peak_template`.
*/
int TemplateFLT::get_sample_peak_template(){
    return this->sample_peak_template;
}

/*
Getter for `sample_peak_template_desampled`.
*/
int TemplateFLT::get_sample_peak_template_desampled(){
    return this->sample_peak_template_desampled;
}

/*
Getter for `corr_window`.
*/
Eigen::Array2i TemplateFLT::get_corr_window(){
    return this->corr_window;
}

/*
Getter for `corr_thresh`.
*/
float TemplateFLT::get_corr_thresh(){
    return this->corr_thresh;
}


/*
-------
METHODS
-------
*/

/*
Loads in templates stored in a txt file.
Stores them into a vector<Eigen::Array> of size N_templates*size_template.

Arguments
---------
`template_file_name` : Path to the txt file storing the templates.

`size_template` : Number of samples of the template. Default is 400.
*/
void TemplateFLT::load_templates(const string& template_file_name,
                                 const int& size_template,
                                 const int& sample_peak_template){
    // Instance of template file
    ifstream template_file(template_file_name);

    // Check if the file opened successfully
    if(!template_file){
        string err_msg = "Error opening template file: " + template_file_name;
        throwError(err_msg,__FILE__,__LINE__);
    }

    // Vector containing templates
    vector< Eigen::ArrayXf > templates;
    // Line of the file being read
    string line;

    // Read each line from the file (= 1 template)
    while( getline(template_file, line) ){
        // Skip lines starting with '#'
        if (line[0] == '#') {
            continue;
        }

        // Stream that reads values on the line
        istringstream iss(line);
        // Value of the template being read
        float value;

        // Template of this line
        Eigen::ArrayXf templ(size_template);

        // Read floating point numbers from the line = samples of the template
        int i = 0;
        while(iss >> value){
            templ[i] = value;
            i += 1;
        }

        // Add the row to the vector of vectors
        templates.push_back( templ );
    }

    // Close the file
    template_file.close();

    // Store the templates in the object
    this->templates = templates;
    this->size_template = size_template;
    this->sample_peak_template = sample_peak_template;

    cout << ">>> Loaded " << templates.size() << " templates of " << size_template << " samples" << endl;

    // Desample the templates
    desample_templates();

    return;
}


/*
Creates a set of desampled templates for each of the original templates.
The desampled templates will be stored in a 3D vector of size N_templates*desampling_factor*size_template_desampled.

We will generally use templates of 400 samples simulated with a time resolution of 0.5 ns (2 GHz).
Data recorded by the ADC has a time resolution of 2 ns (500 MHz). In this case, desampling_factor = 4.
Therefore, for each template, we can create 4 sub-templates of 100 samples, each with a time resolution of 0.5 ns.
*/
void TemplateFLT::desample_templates(){
    // Check that desampling factor >= 1
    if (desampling_factor < 1){
        string err_msg = "Desampling factor " + to_string(desampling_factor) + " has to be >= 1!";
        throwError(err_msg,__FILE__,__LINE__);
    }

    // Check that templates have been loaded
    if (templates.size() < 1){
        ostringstream oss;
        oss << "No templates have been loaded yet!" << endl;
        throw runtime_error( oss.str() );
    }

    vector< vector< Eigen::ArrayXf > > templates_desampled;

    // Loop over all original templates
    for (int i=0; i<templates.size(); i++){
        vector< Eigen::ArrayXf > desampled_template_set;

        // Loop over all possible desampling indices of one template
        for (int j=0; j<desampling_factor; j++){
            // Initiate desampled array

            // Create desampled array 
            Eigen::ArrayXf desampled_template = templates[i]( Eigen::seq(j,
                                                                         templates[i].size()-1,
                                                                         desampling_factor) );

            desampled_template_set.push_back(desampled_template);
        }
        templates_desampled.push_back(desampled_template_set);
    }
    
    // Store the desampled templates in the object
    this->templates_desampled = templates_desampled;
    this->size_template_desampled = this->size_template / this->desampling_factor;
    this->sample_peak_template_desampled = this->sample_peak_template / this->desampling_factor;

    cout << ">>> Split each template of " << this->size_template << " samples" << endl; 
    cout << "    at simulated sampling rate of " << this->sim_sampling_rate << " MHz" << endl;
    cout << "    into " << this->desampling_factor << " desampled templates of " << this->size_template_desampled << " samples" << endl;
    cout << "    at ADC sampling rate of " << this->adc_sampling_rate << " MHz" << endl;

    return;
}


/*
Computes the maximum abs(correlation) value of a trace and a template in the specified correlation window.

Arguments
---------
`trace` : Input ADC trace.

`templ` : Template with same sampling rate as `trace`.

`norm` : Option to normalize the maximum abs(correlation) value between [0,1]. Default is true.

Returns
-------
`result` : Result tuple containing:
0-> `t_best` : The sample of `trace` yielding the maximum correlation with `templ`.
1-> `corr_max` : The maximum correlation value of `trace` with `templ`.
*/
tuple<int,float> TemplateFLT::compute_max_correlation(const Eigen::ArrayXi& trace,
                                                      const Eigen::ArrayXf& templ,
                                                      const bool& norm){     
    // All correlation values (in absolute value) for the template and the trace
    // Normalized between [0,1] by default
    Eigen::ArrayXf correlations_abs = correlate(trace.cast<float>(),templ,norm).abs();

    // The best-fit time = sample of trace with largest correlation
    int t_best;
    
    // Maximum correlation value
    float corr_max = correlations_abs.maxCoeff(&t_best);

    //cout << rms(trace.cast<float>()) <<endl;
    // Tuple containing the best-fit time and corresponding maximum correlation
    tuple<int,float> result(t_best,corr_max);

    return result;
}


/*
Performs the template fit for a trace.
For each template, the maximum correlation is computed in a window around the trace maximum.
The template that yields the largest correlation is tagged as the best-fit template.

Arguments
---------
`trace` : Input ADC trace.

`t_max` : Position of the trace maximum around which `this->corr_window` will be centered.
*/
void TemplateFLT::template_fit(const Eigen::ArrayXi& trace,
                               const int& t_max){

    // Trace segment for which the correlation will be computed
    Eigen::ArrayXi trace_segment;

    // Size of the segment
    // Correlation window size + number of samples of desampled template
    int size_segment = ( corr_window(1) - corr_window(0) ) + (size_template_desampled);
    //cout<<size_segment<<endl;
    
    // Starting sample of the segment
    // Sample of trace maximum - sample of template maximum
    // This way the peaks of the trace and template "overlap"
    int sample_start_segment = t_max - this->sample_peak_template_desampled;
    
    // Get the relevant trace segment 
    // Patch if window falls at the start of the trace
    if (sample_start_segment < 0){
        trace_segment = trace.segment(0,size_segment+sample_start_segment);
    }
    // Patch if the window is at the end of the trace
    else if (sample_start_segment + size_segment > trace.size() - 1){
        trace_segment = trace.segment(sample_start_segment,trace.size());
    }
    else{
        trace_segment = trace.segment(sample_start_segment,size_segment);
    }

    // ID of best-fit template
    int template_id_best;

    // Index of the best desampling of the best-fit template and of template i
    int idx_template_desampled_best, idx_template_desampled_best_i;

    // Best-fit time overall, of template i, and of desampled template j of template i
    int t_best, t_best_i, t_best_ij;

    // Maximum correlation of the trace with all templates, with template i, and with desampled template j of template i
    float corr_max, corr_max_i, corr_max_ij;

    // Loop over all templates i
    corr_max = 0;
    for (int i=0; i<templates.size(); i++){
        // Loop over all desamplings j of template i
        corr_max_i = 0;
        for (int j=0; j<desampling_factor; j++){
            // Compute the maximum correlation of the trace segment and desampled template j of template i
            tie(t_best_ij,corr_max_ij) = compute_max_correlation(trace_segment,templates_desampled[i][j]);
            // The max corr of template i is picked as the max corr of all its desampled templates j
            if (corr_max_ij > corr_max_i){
                idx_template_desampled_best_i = j;
                t_best_i = t_best_ij;
                corr_max_i = corr_max_ij;
            }
        }
        // The max corr overall is picked as the max corr of all templates i
        if (corr_max_i > corr_max){
            template_id_best = i;
            idx_template_desampled_best = idx_template_desampled_best_i;
            t_best = t_best_i;
            corr_max = corr_max_i;
        }
    }

    // Store the template-fit results in the object
    this->template_id_best = template_id_best;
    this->idx_template_desampled_best = idx_template_desampled_best;
    this->t_peak_best = t_best + sample_start_segment + this->sample_peak_template_desampled;
    this->corr_max_best = corr_max;

    return;
}


/*
Trigger TODO
*/
bool TemplateFLT::trigger(const Eigen::ArrayXi& trace){
    // INPUT FLT-0 PARAMETERS HERE
    // TRIGGER TIME AND FIRST T1 CROSSING TIME

    // Perform the template fit
    // this->template_fit(trace,t_max)

    // Decision to trigger
    bool decision;

    if (this->corr_max_best > this->corr_thresh){
        decision = true;
    }
    else{
        decision = false;
    }

    return decision;
}