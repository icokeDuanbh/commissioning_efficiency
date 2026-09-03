////////////////////////////
//** UTILS SOURCE FILE ** //
////////////////////////////

#include "utils.h"
#include "error_handling.h"

using namespace std;

/*
---------
FUNCTIONS
---------
*/

/*
Computes the cross correlation of two Eigen::Arrays. 
Mimics the `numpy.correlate` function of Python in "valid" mode.
Implemented with the help of ChatGPT.

Arguments
---------
`arr1` : First array of size N.

`arr2` : Second array of size M <= N. This is the one you "slide" along the first array.

`norm` : Option to normalize the correlation between [-1,1]. Based on REF.

Returns
-------
`corr` : The cross correlation of arr1 with arr2. Its size is N - M + 1.
*/
Eigen::ArrayXf correlate(const Eigen::ArrayXf& arr1,
                         const Eigen::ArrayXf& arr2,
                         const bool& norm){
    // Ensure that arr1 is larger than or equal to arr2
    if (arr1.size() < arr2.size()){
        string err_msg = "Invalid argument: arr1=" + to_string(arr1.size()) + " must be >= arr2=" + to_string(arr2.size());
        throwError(err_msg,__FILE__,__LINE__);
    }

    size_t n = arr1.size();
    size_t m = arr2.size();
    size_t corr_size = n - m + 1;

    // Initiate correlation array
    Eigen::ArrayXf corr(corr_size);
    
    // Compute the cross correlation in "valid" mode
    for (size_t i = 0; i < corr_size; ++i){
        // Perform element-wise multiplication and summing
        corr(i) = arr1.segment(i,m).matrix().dot( arr2.matrix() );

        // Normalize the correlation with stepwize RMS of arr1 segment
        if (norm){
            corr(i) /= rms( arr1.segment(i,m) );
        }
    }

    // Normalize all correlation with RMS of arr2 and its length to yield a value between [-1,1]
    if (norm){
        corr / rms(arr2) / arr2.size();
    }

    return corr;
}


/*
Computes the root mean square (RMS) of an array.

Arguments
---------
`arr` : The array of which to compute the RMS.

Returns
-------
`rms` : The RMS of the input array.
*/
float rms(const Eigen::ArrayXf& arr){
    // Compute the RMS
    float rms = sqrt( arr.square().mean() );

    return rms;
}


/*
Normalizes an array between [-1,1], i.e. with respect to the maximum value of abs(array).

Arguments
---------
`arr` : The array to normalize.

Returns
-------
`arr_norm` : The normalized array.
*/
Eigen::ArrayXf normalize(const Eigen::ArrayXf& arr){
    // Normalize the array
    Eigen::ArrayXf arr_norm = arr/arr.abs().maxCoeff();

    return arr_norm;
}


vector<Eigen::ArrayXi> load_test_trace(string test_trace_file_name){

    vector<Eigen::ArrayXi> trace_3D;

    ifstream test_trace_file(test_trace_file_name);
    string line;

    for (int k = 0; k < 3 && getline(test_trace_file,line); k++){
        
        Eigen::ArrayXi trace(1024);
        
        istringstream iss(line);
        int value;

        int i = 0;
        while(iss>>value){
            trace[i] = value;
            i += 1;
        }

        trace_3D.push_back(trace);
    }
    
    return trace_3D;
}