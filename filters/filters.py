import numpy as np
import scipy.signal as signal
import re
import xml.etree.ElementTree as ET

def get_coefficients_from_xci(xci_path="/Users/xishui/Dropbox/Project/GRAND/Event_injector/filters/fir_compiler_lp_120M.xci"):
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


coef_float = get_coefficients_from_xci()
def apply_fir_filter(x, coef=coef_float):
    """Apply the FIR filter (low pass filter below 120MHz)

    Parameters:
    -----------
    x: np.ndarray, int32
    the signal to be processed

    coef: float
    the coefficients of the FIR filter

    Returns:
    --------
    y_int_truncated_shifted: np.ndarray, int32
    The filtered array
    """
    coefs_int = np.round(coef * 65536).astype(np.float64)
    x_float = x.astype(np.float64)
    # Integer filter (exact integer math represented in float64 to avoid SciPy's lack of int64 support)
    y_int = signal.lfilter(coefs_int, 1, x_float)
    # Scale back down by 2^16 (65536)
    y_int_scaled_truncated = (y_int // 65536).astype(np.int32)
    latency = 49 + 11 # Empirical shift to match the online/offline filters
    
    y_int_truncated_shifted = np.zeros_like(x, dtype=int)
    y_int_truncated_shifted[latency:] = y_int_scaled_truncated[:-latency]
    return y_int_truncated_shifted


def compute_notch_coefficients(fn, r, fs=500e6):
    """
    Compute notch filter coefficients.
    
    Args:
        fn: Notch frequency in Hz (e.g. 100e6 for 100 MHz)
        r:  Notch width parameter (0 < r < 1)
        fs: Sampling frequency in Hz (default 500 MHz)
    
    Returns:
        dict of coefficients a1, a2, b1, b2, b3, b4, b5, b6
    """
    nu = 2 * np.pi * fn / fs

    a1 = 2 * (r**4) * np.cos(4 * nu)
    a2 = -(r**8)
    b1 = -2 * np.cos(nu)
    b2 = 1
    b3 = 2 * r * np.cos(nu)
    b4 = r * r
    b5 = 2 * (r**2) * np.cos(2 * nu)
    b6 = r**4

    return dict(a1=a1, a2=a2, b1=b1, b2=b2, b3=b3, b4=b4, b5=b5, b6=b6)


def apply_notch_filter(X, fn, r, fs=500e6):
    """
    Apply the notch filter to input signal X.
    
    Args:
        X:  Input signal (list or numpy array)
        fn: Notch frequency in Hz
        r:  Notch width parameter (0 < r < 1)
        fs: Sampling frequency in Hz (default 500 MHz)
    
    Returns:
        Y1, Y2, Y: Intermediate and final filtered signals as numpy arrays
    """
    c = compute_notch_coefficients(fn, r, fs)
    a1, a2 = c['a1'], c['a2']
    b1, b2, b3, b4, b5, b6 = c['b1'], c['b2'], c['b3'], c['b4'], c['b5'], c['b6']

    N = len(X)
    Y1 = np.zeros(N)
    Y2 = np.zeros(N)
    Y  = np.zeros(N)

    for n in range(N):
        # Y1[n] = b2 * X[n] + b1 * X[n-1] + X[n-2]
        x_n1 = X[n - 1] if n >= 1 else 0
        x_n2 = X[n - 2] if n >= 2 else 0
        Y1[n] = b2 * X[n] + b1 * x_n1 + x_n2

        # Y2[n] = Y1[n] + b3 * Y1[n-1] + b4 * Y1[n-2]
        y1_n1 = Y1[n - 1] if n >= 1 else 0
        y1_n2 = Y1[n - 2] if n >= 2 else 0
        Y2[n] = Y1[n] + b3 * y1_n1 + b4 * y1_n2

        # Y[n] = a1*Y[n-4] + a2*Y[n-8] + Y2[n-2] + b5*Y2[n-4] + b6*Y2[n-6]
        y_n4  = Y[n - 4] if n >= 4 else 0
        y_n8  = Y[n - 8] if n >= 8 else 0
        y2_n2 = Y2[n - 2] if n >= 2 else 0
        y2_n4 = Y2[n - 4] if n >= 4 else 0
        y2_n6 = Y2[n - 6] if n >= 6 else 0
        Y[n] = a1 * y_n4 + a2 * y_n8 + y2_n2 + b5 * y2_n4 + b6 * y2_n6

    return Y # Only returning the filtered trace


# --- Example usage ---
if __name__ == "__main__":
    # Generate a test signal: mix of 50 MHz (keep) + 100 MHz (notch out)
    fs = 500e6
    t = np.arange(1024) / fs
    X = np.sin(2 * np.pi * 50e6 * t) + np.sin(2 * np.pi * 100e6 * t)

    # Apply notch at 100 MHz with width r=0.95
    Y = apply_notch_filter(X, fn=100e6, r=0.95, fs=fs)
    
    # Print the computed coefficients
    coefs = compute_notch_coefficients(fn=100e6, r=0.95, fs=fs)
    print("Notch Filter Coefficients (fn=100 MHz, r=0.95, fs=500 MHz):")
    for k, v in coefs.items():
        print(f"  {k} = {v:.6f}")

