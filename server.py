import asyncio
import websockets
import socket
import sys
import io
import json
import time
import numpy as np
import csv
from collections import deque
from scipy.signal import butter, filtfilt

msg_count = 0
last_print = time.time()
last_fft_time = 0  # To prevent FFT spam

# Sample tracking globals
sample_count = 0
packet_count = 0
start_time = time.time()

# CSV globals
csv_file = None
csv_writer = None
csv_filename = ""

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

HOST = "0.0.0.0"
PORT = 1234

connected_clients = set()

# ===== EEG FFT CONFIGURATION =====
EEG_FS = 100                 # sampling rate
FFT_WINDOW = 256             # 2.56 seconds

eeg_buffer = deque(maxlen=FFT_WINDOW)

def bandpass_filter(eeg):
    b, a = butter(
        N=4,
        Wn=[1, 40],
        btype='bandpass',
        fs=EEG_FS
    )
    return filtfilt(b, a, eeg)

def compute_eeg_bands(samples):
    eeg = np.array(samples, dtype=np.float32)

    # remove DC offset
    eeg = eeg - np.mean(eeg)
    
    # Apply 1-40 Hz bandpass filter
    eeg = bandpass_filter(eeg)

    fft = np.fft.rfft(eeg)

    freqs = np.fft.rfftfreq(
        len(eeg),
        d=1.0 / EEG_FS
    )

    power = np.abs(fft) ** 2

    bands = {
        "delta": (0.5, 4),
        "theta": (4, 8),
        "alpha": (8, 13),
        "beta":  (13, 30),
        "gamma": (30, 40) # Capped at 40 Hz due to filter
    }

    result = {}

    for name, (f1, f2) in bands.items():
        mask = (freqs >= f1) & (freqs < f2)
        result[name] = float(np.sum(power[mask]))

    return result

async def handler(websocket):
    global msg_count, last_print, last_fft_time
    global sample_count, packet_count, start_time

    client_ip = websocket.remote_address
    connected_clients.add(websocket)

    print(f"[+] Client connected: {client_ip}", flush=True)
    print(f"    Total clients: {len(connected_clients)}", flush=True)

    try:
        async for message in websocket:

            # ===== MESSAGE RATE COUNTER =====
            msg_count += 1

            if time.time() - last_print >= 1:
                elapsed = time.time() - start_time
                expected = elapsed * EEG_FS
                loss = expected - sample_count

                print(
                    f"RX/s={msg_count} | "
                    f"Packets={packet_count} | "
                    f"Samples={sample_count} | "
                    f"Expected={expected:.0f} | "
                    f"Loss={loss:.0f}",
                    flush=True
                )
                msg_count = 0
                last_print = time.time()

            # ===== JSON PARSE =====
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                print("    [!] Invalid JSON, skipping", flush=True)
                continue

            # ===== EEG BUFFERING & FFT =====
            if data.get("dev") == "EEG":
                samples = data.get("samples", [])

                packet_count += 1

                # Print every 20th packet to avoid terminal spam
                if packet_count % 20 == 0:
                    print(f"Packet length = {len(samples)}")
                    print(f"First 20 samples = {samples[:20]}")

                for sample in samples:
                    if sample ==0:
                        continue
                    
                    sample_count += 1
                    eeg_buffer.append(sample)
                    
                    if csv_writer is not None:
                        csv_writer.writerow([
                            sample_count,
                            time.time(),
                            sample
                        ])

                # Only compute FFT if buffer is full AND 1 second has passed
                if len(eeg_buffer) == FFT_WINDOW and (time.time() - last_fft_time > 1.0):
                    last_fft_time = time.time()
                    
                    # 1. Compute Bands
                    bands = compute_eeg_bands(eeg_buffer)

                    # 2. Alpha Detector
                    total_power = (
                        bands["delta"]
                        + bands["theta"]
                        + bands["alpha"]
                        + bands["beta"]
                        + bands["gamma"]
                    )

                    if total_power > 0:
                        alpha_ratio = (bands["alpha"] / total_power)
                    else:
                        alpha_ratio = 0.0
                    if bands["alpha"] > 0:
                        beta_alpha_ratio = (bands["beta"] / bands["alpha"] )
                    else:
                         beta_alpha_ratio = 0.0
                    
                    print(
                        f"Δ={bands['delta']:.0f} "
                        f"Θ={bands['theta']:.0f} "
                        f"Α={bands['alpha']:.0f} "
                        f"Β={bands['beta']:.0f} "
                        f"Γ={bands['gamma']:.0f} | "
                        f"Alpha Ratio = {alpha_ratio:.3f} | "
                        f"Beta/Alpha={beta_alpha_ratio:.3f}",
                        flush=True
                    )

                    # 3. Broadcast EEG_BANDS 
                    band_msg = {
                        "dev": "EEG_BANDS",
                        **bands,
                        "alpha_ratio": float(alpha_ratio),
                        "beta_alpha_ratio": float(beta_alpha_ratio)
                    }
                    band_json = json.dumps(band_msg)

                    for client in connected_clients.copy():
                        if client != websocket:
                            try:
                                await client.send(band_json)
                            except:
                                pass
                    
                    # 4. Broadcast Full Spectrum FFT
                    eeg_arr = np.array(eeg_buffer, dtype=np.float32)
                    eeg_arr = eeg_arr - np.mean(eeg_arr)
                    
                    # Apply 1-40 Hz bandpass filter
                    eeg_arr = bandpass_filter(eeg_arr)
                    
                    # Apply Hamming window to reduce spectral leakage
                    window = np.hamming(len(eeg_arr))
                    fft_vals = np.fft.rfft(eeg_arr * window)
                    
                    freqs = np.fft.rfftfreq(len(eeg_arr), d=1.0 / EEG_FS)
                    power = np.abs(fft_vals) ** 2  # Use Power Spectrum

                    # Normalize FFT power (0 to 1 scaling)
                    if np.max(power) > 0:
                        power = power / np.max(power)

                    # Limit to 50 Hz (Nyquist)
                    mask = freqs <= 50
                    freqs = freqs[mask]
                    power = power[mask]

                    # Downsample by 2 to smooth browser rendering
                    freqs = freqs[::2]
                    power = power[::2]

                    # Print resolution for debugging
                    resolution = EEG_FS / FFT_WINDOW
                    # print(f"FFT Resolution = {resolution:.3f} Hz/bin", flush=True)

                    fft_msg = {
                        "dev": "FFT",
                        "freq": freqs.tolist(),
                        "power": power.tolist()
                    }
                    fft_json = json.dumps(fft_msg)

                    for client in connected_clients.copy():
                        if client != websocket:
                            try:
                                await client.send(fft_json)
                            except:
                                pass

            # ===== FORWARD TO OTHER CLIENTS (Raw Data) =====
            for client in connected_clients.copy():
                if client != websocket:
                    try:
                        await client.send(message)
                    except:
                        pass

    except websockets.exceptions.ConnectionClosed as e:
        print(f"[-] Client disconnected: {client_ip} | Reason: {e}", flush=True)

    finally:
        connected_clients.discard(websocket)
        print(f"    Remaining clients: {len(connected_clients)}", flush=True)
        if csv_file is not None:
            csv_file.flush()

async def main():
    # Fetch real local IP using a UDP socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
    except Exception:
        local_ip = "127.0.0.1"
    finally:
        s.close()

    print("=" * 60, flush=True)
    print("WebSocket Server Starting", flush=True)
    print(f"Listening on: ws://{HOST}:{PORT}", flush=True)
    print(f"Your PC IP: {local_ip}", flush=True)
    print("=" * 60, flush=True)
    print("Waiting for connections...\n", flush=True)

    async with websockets.serve(handler, HOST, PORT, ping_interval=20, ping_timeout=10):
        await asyncio.Future()

if __name__ == "__main__":
    # Setup CSV writing
    filename = input("CSV filename (without .csv): ").strip()
    csv_filename = filename + ".csv"
    
    csv_file = open(csv_filename, "w", newline="")
    csv_writer = csv.writer(csv_file)
    csv_writer.writerow(["sample_index", "timestamp", "eeg"])
    
    # Reset start time here so time spent typing doesn't throw off expected loss
    start_time = time.time()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[!] Server stopped", flush=True)
        if csv_file:
            csv_file.close()
        print(f"\nRecorded {sample_count} samples to {csv_filename}", flush=True)