import serial
import numpy as np
import matplotlib.pyplot as plt
from collections import deque
from scipy.signal import butter, filtfilt

# ==========================
# CONFIG
# ==========================

PORT = "/dev/ttyACM0"     # Change if needed
BAUD = 115200

FS = 256                  # EEG example sample rate
WINDOW = 1024             # 4 seconds

# ==========================
# SERIAL
# ==========================

ser = serial.Serial(PORT, BAUD, timeout=1)

buffer = deque(maxlen=WINDOW)

plt.ion()

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8))

time_line, = ax1.plot([], [])
fft_line, = ax2.plot([], [])

ax1.set_title("EEG Time Domain")
ax1.set_ylabel("Amplitude")

ax2.set_title("FFT")
ax2.set_xlabel("Frequency (Hz)")
ax2.set_ylabel("Magnitude")

while True:
    try:
        line = ser.readline().decode().strip()

        if not line:
            continue

        value = float(line)

        buffer.append(value)

        if len(buffer) < WINDOW:
            continue

        eeg = np.array(buffer)

        # ------------------
        # Time Domain Plot
        # ------------------

        time_line.set_data(
            np.arange(len(eeg)),
            eeg
        )

        ax1.set_xlim(0, len(eeg))
        ax1.set_ylim(
            np.min(eeg) - 10,
            np.max(eeg) + 10
        )

        # ------------------
        # FFT
        # ------------------

        eeg = eeg - np.mean(eeg)

        window = np.hamming(len(eeg))

        fft_vals = np.fft.rfft(eeg * window)

        freqs = np.fft.rfftfreq(
            len(eeg),
            d=1.0 / FS
        )

        power = np.abs(fft_vals)

        fft_line.set_data(freqs, power)

        ax2.set_xlim(0, 50)

        ax2.set_ylim(
            0,
            np.max(power) * 1.1
        )

        plt.pause(0.01)

    except KeyboardInterrupt:
        break

    except Exception as e:
        print(e)