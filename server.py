import asyncio
import websockets
import socket
import sys
import io
import json
import time
import numpy as np
import csv
import re
from collections import deque
from scipy.signal import butter, filtfilt

# ===== GLOBALS =====
msg_count = 0
last_print = time.time()
last_fft_time = 0
sample_count = 0
last_sample_count = 0
packet_count = 0
start_time = time.time()
imu_string_buffer = ""
csv_file = None
csv_writer = None
csv_filename = ""

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
HOST = "0.0.0.0"
PORT = 1234
connected_clients = set()

# ===== EEG FFT CONFIGURATION =====
EEG_FS = 100
FFT_WINDOW = 256
eeg_buffer = deque(maxlen=FFT_WINDOW)

def bandpass_filter(eeg):
    b, a = butter(N=4, Wn=[1, 40], btype='bandpass', fs=EEG_FS)
    return filtfilt(b, a, eeg)

def compute_eeg_bands(samples):
    eeg = np.array(samples, dtype=np.float32)
    eeg = eeg - np.mean(eeg)
    eeg = bandpass_filter(eeg)
    fft = np.fft.rfft(eeg)
    freqs = np.fft.rfftfreq(len(eeg), d=1.0 / EEG_FS)
    power = np.abs(fft) ** 2
    bands = {"delta": (0.5, 4), "theta": (4, 8), "alpha": (8, 13), "beta": (13, 30), "gamma": (30, 40)}
    result = {}
    for name, (f1, f2) in bands.items():
        mask = (freqs >= f1) & (freqs < f2)
        result[name] = float(np.sum(power[mask]))
    return result

async def handler(websocket):
    global msg_count, last_print, last_fft_time, sample_count, last_sample_count, packet_count, start_time, imu_string_buffer

    client_ip = websocket.remote_address
    connected_clients.add(websocket)
    print(f"[+] Client connected: {client_ip}", flush=True)

    try:
        async for message in websocket:
            # ===== KEY FIX: Parse batch - get ALL messages not just first =====
            try:
                data = json.loads(message)
                # Handle both [msg1, msg2, ...] and single msg
                messages = data if isinstance(data, list) else [data]
            except (json.JSONDecodeError, AttributeError, IndexError):
                continue
            
            # ===== Process EVERY message in the batch =====
            for msg in messages:
                forward_raw_message = True
                
                try:
                    dev_type = msg.get("dev")
                except (AttributeError, TypeError):
                    continue
                    
                # ===== EEG PROCESSING =====
                if dev_type == "EEG":
                    samples = msg.get("samples", [])
                    packet_count += 1
                    for sample in samples:
                        if sample == 0: continue
                        sample_count += 1
                        eeg_buffer.append(sample)
                        if csv_writer: csv_writer.writerow([sample_count, time.time(), sample])

                    if len(eeg_buffer) == FFT_WINDOW and (time.time() - last_fft_time > 1.0):
                        last_fft_time = time.time()
                        bands = compute_eeg_bands(eeg_buffer)
                        total_power = sum(bands.values())
                        alpha_ratio = (bands["alpha"] / total_power) if total_power > 0 else 0.0
                        
                        print(f"[FFT] Α={bands['alpha']:.0f} | Alpha Ratio={alpha_ratio:.3f}", flush=True)

                        band_msg = {"dev": "EEG_BANDS", **bands, "alpha_ratio": float(alpha_ratio)}
                        
                        for client in connected_clients.copy():
                            if client != websocket:
                                try: await client.send(json.dumps(band_msg))
                                except: pass

                # ===== IMU PROCESSING =====
                elif dev_type == "IMU" and "raw" in msg:
                    forward_raw_message = False
                    imu_string_buffer += msg["raw"]
                    match = re.search(r"AX:([\-\d\.]+)\s*AY:([\-\d\.]+)\s*AZ:([\-\d\.]+)\s*GX:([\-\d\.]+)\s*GY:([\-\d\.]+)\s*GZ:([\-\d\.]+)", imu_string_buffer)
                    if match:
                        clean_imu_msg = json.dumps({"dev": "IMU", "ax": match.group(1), "ay": match.group(2), "az": match.group(3), "gx": match.group(4), "gy": match.group(5), "gz": match.group(6)})
                        for client in connected_clients.copy():
                            if client != websocket:
                                try: await client.send(clean_imu_msg)
                                except: pass
                        imu_string_buffer = imu_string_buffer[match.end():]

                if forward_raw_message:
                    for client in connected_clients.copy():
                        if client != websocket:
                            try: await client.send(json.dumps(msg))
                            except: pass

            # ===== RATE COUNTER =====
            if time.time() - last_print >= 1.0:
                actual_hz = sample_count - last_sample_count
                print(f"[EEG STATS] Actual Rate: {actual_hz} Hz | Total Packets: {packet_count}", flush=True)
                last_print, last_sample_count = time.time(), sample_count

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(websocket)

async def main():
    async with websockets.serve(handler, HOST, PORT, ping_interval=20, ping_timeout=10):
        await asyncio.Future()

if __name__ == "__main__":
    filename = input("CSV filename (without .csv): ").strip()
    csv_file = open(filename + ".csv", "w", newline="")
    csv_writer = csv.writer(csv_file)
    csv_writer.writerow(["sample_index", "timestamp", "eeg"])
    try: asyncio.run(main())
    except KeyboardInterrupt:
        if csv_file: csv_file.close()