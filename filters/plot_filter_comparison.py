import numpy as np
import matplotlib.pyplot as plt

def plot_traces(npz_path, fs_mhz=500.0):
    # 1. Load the data
    data = np.load(npz_path)
    ch0 = data['ch0']  # Filtered trace
    ch2 = data['ch2']  # Unfiltered trace
    
    n_samples = len(ch2)
    t_step_ns = 1000.0 / fs_mhz  # 2 ns for 500 MHz
    time_ns = np.arange(n_samples) * t_step_ns
    
    # 2. Compute Frequency Domain (FFT)
    # Using rfft for real-valued signals
    fft_ch0 = np.fft.rfft(ch0)
    fft_ch2 = np.fft.rfft(ch2)
    
    # Frequency bins in MHz
    freqs_mhz = np.fft.rfftfreq(n_samples, d=t_step_ns * 1e-9) / 1e6
    
    # Calculate magnitude in dB (adding a small epsilon to avoid log(0))
    mag_ch0 = 20 * np.log10(np.abs(fft_ch0) + 1e-5)
    mag_ch2 = 20 * np.log10(np.abs(fft_ch2) + 1e-5)
    
    # Normalize dB to peak of unfiltered channel for easier visual comparison
    peak_db = np.max(mag_ch2)
    mag_ch0_norm = mag_ch0 - peak_db
    mag_ch2_norm = mag_ch2 - peak_db

    # 3. Create the plots
    plt.style.use('seaborn-v0_8-whitegrid' if 'seaborn-v0_8-whitegrid' in plt.style.available else 'default')
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=False)
    
    # --- TIME DOMAIN PLOT ---
    ax1.plot(time_ns, ch2, color='#ff7f0e', alpha=0.8, linewidth=1.5, label='CH2: Unfiltered')
    ax1.plot(time_ns, ch0, color='#1f77b4', alpha=0.9, linewidth=1.5, label='CH0: Filtered')
    ax1.set_title('Time Domain Waveform Comparison', fontsize=14, fontweight='bold', pad=10)
    ax1.set_xlabel('Time (ns)', fontsize=12)
    ax1.set_ylabel('Amplitude (ADC Counts)', fontsize=12)
    ax1.legend(loc='upper right', frameon=True, facecolor='white', framealpha=0.9)
    ax1.set_xlim(time_ns[0], time_ns[-1])
    ax1.grid(True, linestyle='--', alpha=0.6)
    
    # --- FREQUENCY DOMAIN PLOT ---
    ax2.plot(freqs_mhz, mag_ch2_norm, color='#ff7f0e', alpha=0.7, linewidth=1.5, label='CH2: Unfiltered')
    ax2.plot(freqs_mhz, mag_ch0_norm, color='#1f77b4', alpha=0.9, linewidth=1.5, label='CH0: Filtered')
    ax2.set_title('Frequency Spectrum Comparison (Normalized)', fontsize=14, fontweight='bold', pad=10)
    ax2.set_xlabel('Frequency (MHz)', fontsize=12)
    ax2.set_ylabel('Magnitude (dB)', fontsize=12)
    ax2.legend(loc='lower left', frameon=True, facecolor='white', framealpha=0.9)
    ax2.set_xlim(0, fs_mhz / 2)  # Nyquist frequency is fs / 2 (250 MHz)
    ax2.set_ylim(-60, 5)        # Zoom in on main spectral range
    ax2.grid(True, linestyle='--', alpha=0.6)
    
    # Highlight the filter cutoff region (nominally 120 MHz)
    ax2.axvline(x=120.0, color='red', linestyle=':', alpha=0.7, label='Cutoff (~120 MHz)')
    ax2.legend(loc='lower left')
    
    plt.tight_layout()
    
    # Save the figure
    output_img_path = 'test_data/filter_comparison.png'
    plt.savefig(output_img_path, dpi=300)
    print(f"Plot saved successfully to: {output_img_path}")
    
    try:
        plt.show()
    except Exception:
        print("Note: Could not open GUI display window (running in headless environment). The plot has been saved to disk.")

if __name__ == '__main__':
    plot_traces('test_data/entry_100_du_1090.npz')
