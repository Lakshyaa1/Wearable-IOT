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

msg_count = 0
last_print = time.time()
last_fft_time = 0  # To prevent FFT spam

# Sample tracking globals
sample_count = 0
packet_count = 0
start_time = time.time()

# Buffer to stitch fragmented IMU strings back together
imu_string_buffer = ""

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
    global imu_string_buffer

    client_ip = websocket.remote_address
    connected_clients.add(websocket)

    print(f"[+] Client connected: {client_ip}", flush=True)
    print(f"    Total clients: {len(connected_clients)}", flush=True)

    try:
        async for message in websocket:
            
            forward_raw_message = True # Flag to control what gets sent to frontend

            # ===== JSON PARSE =====
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                print("    [!] Invalid JSON, skipping", flush=True)
                continue
                
            dev_type = data.get("dev")

            # ===== EEG PROCESSING =====
            if dev_type == "EEG":
                msg_count += 1 
                samples = data.get("samples", [])
                packet_count += 1

                if packet_count % 20 == 0:
                    print(f"[EEG] Packet length = {len(samples)} | First 20 = {samples[:20]}")

                for sample in samples:
                    if sample == 0:
                        continue
                    
                    sample_count += 1
                    eeg_buffer.append(sample)
                    
                    if csv_writer is not None:
                        csv_writer.writerow([
                            sample_count,
                            time.time(),
                            sample
                        ])

                if len(eeg_buffer) == FFT_WINDOW and (time.time() - last_fft_time > 1.0):
                    last_fft_time = time.time()
                    
                    bands = compute_eeg_bands(eeg_buffer)

                    total_power = sum(bands.values())
                    alpha_ratio = (bands["alpha"] / total_power) if total_power > 0 else 0.0
                    beta_alpha_ratio = (bands["beta"] / bands["alpha"]) if bands["alpha"] > 0 else 0.0
                    
                    print(
                        f"[FFT] Δ={bands['delta']:.0f} Θ={bands['theta']:.0f} "
                        f"Α={bands['alpha']:.0f} Β={bands['beta']:.0f} Γ={bands['gamma']:.0f} | "
                        f"Alpha Ratio = {alpha_ratio:.3f} | Beta/Alpha={beta_alpha_ratio:.3f}",
                        flush=True
                    )

                    band_msg = {
                        "dev": "EEG_BANDS",
                        **bands,
                        "alpha_ratio": float(alpha_ratio),
                        "beta_alpha_ratio": float(beta_alpha_ratio)
                    }
                    
                    eeg_arr = np.array(eeg_buffer, dtype=np.float32)
                    eeg_arr = eeg_arr - np.mean(eeg_arr)
                    eeg_arr = bandpass_filter(eeg_arr)
                    
                    window = np.hamming(len(eeg_arr))
                    fft_vals = np.fft.rfft(eeg_arr * window)
                    freqs = np.fft.rfftfreq(len(eeg_arr), d=1.0 / EEG_FS)
                    power = np.abs(fft_vals) ** 2 

                    if np.max(power) > 0:
                        power = power / np.max(power)

                    mask = freqs <= 50
                    freqs = freqs[mask][::2]
                    power = power[mask][::2]

                    fft_msg = {
                        "dev": "FFT",
                        "freq": freqs.tolist(),
                        "power": power.tolist()
                    }

                    for client in connected_clients.copy():
                        if client != websocket:
                            try:
                                await client.send(json.dumps(band_msg))
                                await client.send(json.dumps(fft_msg))
                            except:
                                pass
                                
            # ===== OTHER SENSORS =====
            elif dev_type == "ECG":
                pass 
            
            elif dev_type == "IMU":
                if "raw" in data:
                    forward_raw_message = False # Don't send broken chunks to the dashboard
                    
                    # 1. Add new fragmented chunk to our Python buffer
                    imu_string_buffer += data["raw"]
                    
                    # 2. Use RegEx to search for a complete reading
                    match = re.search(r"AX:([\-\d\.]+)\s*AY:([\-\d\.]+)\s*AZ:([\-\d\.]+)\s*GX:([\-\d\.]+)\s*GY:([\-\d\.]+)\s*GZ:([\-\d\.]+)", imu_string_buffer)
                    
                    # 3. If a full set of 6 numbers is found, extract them
                    if match:
                        try:
                            ax, ay, az, gx, gy, gz = match.groups()
                            
                            # Attempt to convert. If dropped packets mashed numbers together, 
                            # this will raise a ValueError and jump to the except block.
                            ax_f, ay_f, az_f = float(ax), float(ay), float(az)
                            gx_f, gy_f, gz_f = float(gx), float(gy), float(gz)
                            
                            print(f"[IMU] Accel:({ax_f}, {ay_f}, {az_f}) Gyro:({gx_f}, {gy_f}, {gz_f})", flush=True)
                            
                            # Send reconstructed, clean JSON to dashboard clients
                            clean_imu_msg = json.dumps({
                                "dev": "IMU",
                                "ax": ax_f, "ay": ay_f, "az": az_f,
                                "gx": gx_f, "gy": gy_f, "gz": gz_f
                            })
                            
                            for client in connected_clients.copy():
                                if client != websocket:
                                    try:
                                        await client.send(clean_imu_msg)
                                    except:
                                        pass
                                        
                        except ValueError:
                            # Catch the crash! Just print a warning and ignore this corrupted reading.
                            print(f"[IMU] Dropped packet caused corrupted values, discarding: {match.group(0)}", flush=True)
                            
                        finally:
                            # ALWAYS clear the buffer up to the end of the matched string, 
                            # whether it succeeded or failed, so we don't get stuck.
                            imu_string_buffer = imu_string_buffer[match.end():]
                    
                    # Keep buffer from growing infinitely if garbage data builds up
                    if len(imu_string_buffer) > 500:
                        imu_string_buffer = imu_string_buffer[-250:]

                else:
                    # In case it ever does parse perfectly on the ESP32
                    print(f"[IMU] Accel:({data.get('ax')}, {data.get('ay')}, {data.get('az')}) Gyro:({data.get('gx')}, {data.get('gy')}, {data.get('gz')})", flush=True)
            
            elif dev_type == "Load":
                print(f"[LOAD] Weight: {data.get('load')}", flush=True)
            elif dev_type == "RedIR":
                print(f"[SPO2] Red: {data.get('Red')} | IR: {data.get('IR')}", flush=True)

            # ===== MESSAGE RATE COUNTER =====
            if time.time() - last_print >= 1:
                elapsed = time.time() - start_time
                expected = elapsed * EEG_FS
                loss = expected - sample_count

                print(
                    f"EEG RX/s={msg_count} | Packets={packet_count} | "
                    f"Samples={sample_count} | Expected={expected:.0f} | Loss={loss:.0f}",
                    flush=True
                )
                msg_count = 0
                last_print = time.time()

            # ===== FORWARD RAW DATA TO OTHER CLIENTS =====
            if forward_raw_message:
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
    filename = input("CSV filename (without .csv): ").strip()
    csv_filename = filename + ".csv"
    
    csv_file = open(csv_filename, "w", newline="")
    csv_writer = csv.writer(csv_file)
    csv_writer.writerow(["sample_index", "timestamp", "eeg"])
    
    start_time = time.time()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[!] Server stopped", flush=True)
        if csv_file:
            csv_file.close()
        print(f"\nRecorded {sample_count} samples to {csv_filename}", flush=True)