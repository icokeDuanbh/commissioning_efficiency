import numpy as np
import scipy.signal as signal
import matplotlib.pyplot as plt
import re
import xml.etree.ElementTree as ET
from filters import apply_notch_filter


def get_coefficients_from_xci(xci_path):
    """Parses the CoefficientVector from the Xilinx XCI file."""
    tree = ET.parse(xci_path)
    root = tree.getroot()
    
    # Namespaces are commonly used in XCI files
    namespaces = {
        'spirit': 'http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009',
        'xilinx': 'http://www.xilinx.com'
    }
    
    # Search for PARAM_VALUE.CoefficientVector
    coef_str = None
    for element in root.findall('.//spirit:configurableElementValue', namespaces):
        ref_id = element.attrib.get('{http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009}referenceId')
        if ref_id == 'PARAM_VALUE.CoefficientVector':
            coef_str = element.text
            break
            
    if not coef_str:
        raise ValueError("Could not find PARAM_VALUE.CoefficientVector in the XCI file.")
        
    # Remove all whitespace and newlines to handle line-wrapped floats correctly
    coef_str = re.sub(r'\s+', '', coef_str)
        
    # Split by comma and convert to floats
    coefs = [float(x) for x in coef_str.split(',') if x]
    return np.array(coefs)

def run_comparison():
    xci_path = 'filters/fir_compiler_lp_120M.xci'
    npz_path = 'test_data/entry_100_du_1090.npz'
    
    # 1. Load data and coefficients
    data = np.load(npz_path)
    ch0_actual = data['ch0']  # Filtered trace (actual)
    ch2_input = data['ch2']   # Unfiltered trace (input to filter)
    
    coefs_float = get_coefficients_from_xci(xci_path)
    print(f"Loaded {len(coefs_float)} coefficients from XCI file.")

    # 1.5 Apply the notch filter
    ch2_input_raw = data['ch2']
    ch2_input = apply_notch_filter(ch2_input_raw, 39e6, 0.9)

    # 2. Quantize coefficients to 16-bit signed integers (represented as float64 for SciPy compatibility)
    coefs_int = np.round(coefs_float * 65536).astype(np.float64)
    ch2_input_float = ch2_input.astype(np.float64)
    
    # 3. Filter using Python (float vs. integer methods)
    y_float = signal.lfilter(coefs_float, 1, ch2_input_float)
    
    # Integer filter (exact integer math represented in float64 to avoid SciPy's lack of int64 support)
    y_int = signal.lfilter(coefs_int, 1, ch2_input_float)
    # Scale back down by 2^16 (65536)
    y_int_scaled_rounded = np.round(y_int / 65536.0).astype(np.int32)
    y_int_scaled_truncated = (y_int // 65536).astype(np.int32)
    
    # 4. Align with latency (49 cycles delay)
    latency = 49 + 11
    
    # Shift signals
    y_float_shifted = np.zeros_like(ch0_actual, dtype=float)
    y_float_shifted[latency:] = y_float[:-latency]
    
    y_int_rounded_shifted = np.zeros_like(ch0_actual, dtype=int)
    y_int_rounded_shifted[latency:] = y_int_scaled_rounded[:-latency]
    
    y_int_truncated_shifted = np.zeros_like(ch0_actual, dtype=int)
    y_int_truncated_shifted[latency:] = y_int_scaled_truncated[:-latency]

    # 5. Analyze the errors (ignoring the startup phase [0:latency] due to latency shift)
    error_float = ch0_actual[latency:] - y_float_shifted[latency:]
    error_rounded = ch0_actual[latency:] - y_int_rounded_shifted[latency:]
    error_truncated = ch0_actual[latency:] - y_int_truncated_shifted[latency:]
    
    rmse_float = np.sqrt(np.mean(error_float**2))
    rmse_rounded = np.sqrt(np.mean(error_rounded**2))
    rmse_truncated = np.sqrt(np.mean(error_truncated**2))
    
    print(f"Comparison Metrics (ignoring first {latency} samples):")
    print(f"  - Float lfilter RMSE: {rmse_float:.4f}")
    print(f"  - 16-bit Int rounded lfilter RMSE: {rmse_rounded:.4f}")
    print(f"  - 16-bit Int truncated lfilter RMSE: {rmse_truncated:.4f}")
    
    # Find exact matching characteristics
    best_matching_y = None
    best_matching_name = ""
    if rmse_rounded < rmse_truncated:
        best_matching_y = y_int_rounded_shifted
        best_matching_name = "16-bit Int (Rounded)"
        print("--> Rounded scaling matches best!")
    else:
        best_matching_y = y_int_truncated_shifted
        best_matching_name = "16-bit Int (Truncated)"
        print("--> Truncated scaling matches best!")

    # 6. Plotting the Comparison
    plt.style.use('~/Dropbox/Config/presentation.mplstyle')
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 8), sharex=True)
    
    # --- Waveform Plot ---
    time_steps = np.arange(len(ch0_actual))
    ax1.plot(time_steps, ch0_actual, color='#1f77b4', marker='.', linewidth=2.0, label='Actual Filtered Data (ch0)')
    ax1.plot(time_steps, best_matching_y, color='#d62728', marker='.', linestyle='--', linewidth=1.5, 
             label=f'Python Emulated ({best_matching_name}, {latency}-cycle shift)')
    ax1.set_title('Waveform Comparison: Actual vs. Python Emulated Filter', fontsize=14, fontweight='bold')
    ax1.set_ylabel('Amplitude (ADC Counts)', fontsize=12)
    ax1.legend(loc='upper right', frameon=True)
    ax1.grid(True, linestyle='--', alpha=0.6)
    
    # Zoom in to a dynamic window to clearly show the overlap (e.g. samples 200 to 400)
    # ax1.set_xlim(200, 500) 
    
    # --- Difference Plot ---
    diff = ch0_actual - best_matching_y
    ax2.plot(time_steps, diff, color='purple', linewidth=1.2, label='Difference (Actual - Emulated)')
    ax2.set_title('Residual Difference / Error (ch0 - Emulated)', fontsize=14, fontweight='bold')
    ax2.set_xlabel('Sample Index', fontsize=12)
    ax2.set_ylabel('Difference (ADC Counts)', fontsize=12)
    ax2.legend(loc='upper right', frameon=True)
    ax2.grid(True, linestyle='--', alpha=0.6)
    ax2.set_ylim(-10, 10)
    
    plt.tight_layout()
    output_img = 'test_data/python_vs_actual_comparison.png'
    plt.savefig(output_img, dpi=300)
    print(f"Saved comparison plot to: {output_img}")
    
    try:
        plt.show()
    except Exception:
        pass

if __name__ == '__main__':
    run_comparison()
