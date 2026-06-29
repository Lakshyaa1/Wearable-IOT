import sys
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import butter, filtfilt, iirnotch

# ── Configuration ─────────────────────────────────────────────────────────────
FS = 250.0            # Sampling rate (Hz)
NOTCH_FREQ = 50.0     # Mains noise frequency to reject (Hz)
NOTCH_Q = 30.0        # Quality factor (higher = narrower notch)
BANDPASS_LOW = 1.0    # Lowest brainwave frequency (Hz)
BANDPASS_HIGH = 40.0  # Highest brainwave frequency (Hz)

def apply_filters(data):
    """Applies a 50Hz Notch filter followed by a 1-40Hz Bandpass filter."""
    
    # 1. Notch filter (targets exactly 50 Hz and removes it)
    b_notch, a_notch = iirnotch(NOTCH_FREQ, NOTCH_Q, FS)
    notched_data = filtfilt(b_notch, a_notch, data)
    
    # 2. Bandpass filter (keeps only 1 Hz to 40 Hz)
    # This also naturally rejects any remaining 50Hz noise, but doing the
    # Notch first helps prevent extreme noise from skewing the bandpass.
    b_band, a_band = butter(4, [BANDPASS_LOW, BANDPASS_HIGH], btype='bandpass', fs=FS)
    filtered_data = filtfilt(b_band, a_band, notched_data)
    
    return filtered_data

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_eeg.py <your_file.csv>")
        print("Example: python analyze_eeg.py myrecording.csv")
        return
    
    csv_file = sys.argv[1]
    print(f"Loading {csv_file}...")
    
    try:
        df = pd.read_csv(csv_file)
    except Exception as e:
        print(f"Error reading file: {e}")
        return
        
    if 'raw_adc' not in df.columns:
        print("Error: 'raw_adc' column not found in CSV. Are you using the correct file?")
        return
        
    # Extract the raw ADC values
    raw_signal = df['raw_adc'].values
    
    # Mean centering (removes the ~2048 DC offset so the wave centers around 0)
    raw_signal = raw_signal - np.mean(raw_signal)
    
    print("Applying 50 Hz Notch filter and 1-40 Hz Bandpass filter...")
    filtered_signal = apply_filters(raw_signal)
    
    # Create a time axis (in seconds)
    time_axis = np.arange(len(raw_signal)) / FS
    
    # ── Compute FFT (Frequency Spectrum) ──────────────────────────────────────
    print("Computing FFT...")
    n = len(filtered_signal)
    freqs = np.fft.rfftfreq(n, d=1.0/FS)
    
    # Calculate magnitude of the FFT
    fft_magnitude = np.abs(np.fft.rfft(filtered_signal))
    
    # ── Plotting ──────────────────────────────────────────────────────────────
    plt.figure(figsize=(14, 8))
    
    # Plot 1: Time Domain (Raw vs Filtered)
    # We only plot the first 4 seconds so you can actually see the waveforms clearly
    plt.subplot(2, 1, 1)
    plot_len = min(int(FS * 4), len(time_axis)) 
    
    plt.plot(time_axis[:plot_len], raw_signal[:plot_len], 
             label='Raw (Massive 50Hz Noise)', alpha=0.4, color='gray')
    plt.plot(time_axis[:plot_len], filtered_signal[:plot_len], 
             label='Filtered (Clean EEG 1-40Hz)', color='blue', linewidth=1.5)
    
    plt.title('Time Domain: Raw vs Filtered Signal (First 4 seconds)')
    plt.xlabel('Time (seconds)')
    plt.ylabel('Amplitude (ADC units)')
    plt.legend(loc="upper right")
    plt.grid(True, alpha=0.3)
    
    # Plot 2: Frequency Domain (FFT)
    plt.subplot(2, 1, 2)
    
    # Only plot up to 60 Hz (no need to see higher frequencies for EEG)
    freq_mask = freqs <= 60
    plt.plot(freqs[freq_mask], fft_magnitude[freq_mask], color='black', linewidth=1)
    
    plt.title('Frequency Domain (FFT of the Filtered Signal)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude')
    
    # Highlight the standard brainwave bands
    bands = {
        'Delta (0.5-4 Hz)':  (0.5, 4,  'red'),
        'Theta (4-8 Hz)':    (4,   8,  'orange'),
        'Alpha (8-13 Hz)':   (8,   13, 'green'),
        'Beta (13-30 Hz)':   (13,  30, 'blue'),
        'Gamma (30-40 Hz)':  (30,  40, 'violet')
    }
    
    for name, (fmin, fmax, color) in bands.items():
        plt.axvspan(fmin, fmax, color=color, alpha=0.15, label=name)
        
    plt.legend(loc="upper right")
    plt.grid(True, alpha=0.3)
    
    plt.tight_layout()
    print("Done! Opening plot window. Close the window to exit the script.")
    plt.show()

if __name__ == "__main__":
    main()
