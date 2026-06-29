# Wearable-IOT: Multi-Sensor Biosensing Platform

A comprehensive embedded systems project implementing a multi-sensor wearable biosensing pipeline spanning firmware, BLE communication, WiFi relay, and real-time cloud dashboard with advanced EEG signal analysis.

**Architecture:** nRF52840 (sensor hub) → ESP32 (BLE relay) → ESP32 (WiFi gateway) → Python WebSocket server → Browser dashboard

---

## Table of Contents

1. [System Architecture](#system-architecture)
2. [Hardware & Sensors](#hardware--sensors)
3. [BLE Communication Protocol](#ble-communication-protocol)
4. [Firmware Sampling Rates](#firmware-sampling-rates)
5. [Python Analysis Pipeline](#python-analysis-pipeline)
6. [EEG Signal Processing](#eeg-signal-processing)
7. [Setup & Deployment](#setup--deployment)
8. [Project Structure](#project-structure)

---

## System Architecture

### End-to-End Data Flow

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        WEARABLE BIOSENSING PIPELINE                      │
└──────────────────────────────────────────────────────────────────────────┘

  nRF52840               ESP32 (Client)      ESP32 (Gateway)    Python Server
  (Wearable)            (BLE Relay)         (WiFi/WebSocket)    + Dashboard
  
┌───────────┐          ┌─────────────┐     ┌──────────────┐   ┌──────────┐
│  Sensors  │ ──BLE──▶ │ BLE Client  │────▶│ WiFi Client  │───▶│ Listener │
│           │  GATT    │             │UART │              │WS  │  Server  │
└───────────┘          └─────────────┘     └──────────────┘   └──────────┘
                                                                      │
                                                                      │
                                                            ┌─────────▼─────────┐
                                                            │  WebSocket Relay  │
                                                            │  (Real-time data) │
                                                            └───────────────────┘
                                                                      │
                                                            ┌─────────▼─────────┐
                                                            │  Browser Dashboard│
                                                            │  (Visualization)  │
                                                            └───────────────────┘
```

### Key Architectural Decisions

- **Dual ESP32 approach:** One handles BLE-to-UART translation (high reliability), the other manages WiFi/WebSocket (avoids WiFi+BLE contention)
- **CSV format for BLE:** Initially chose CSV over binary to simplify debugging; binary struct recommended for production
- **FreeRTOS queues:** Prevents data loss during WiFi disconnections and handles burst traffic
- **Heartbeat mechanism:** Detects stale connections; LED indicators provide visual system status
- **Queue overflow recovery:** Automatic reconnection and graceful degradation when buffers saturate

---

## Hardware & Sensors

### Primary Sensor Hub: nRF52840 (Wearable)

| Sensor | Interface | Measurement | Resolution | Notes |
|--------|-----------|-------------|-----------|-------|
| **BioAmp EXG Pill** | Analog (ADC) | EEG, ECG, EMG | 10-bit ADC | Upside Down Labs; frontal electrode placement for drowsiness detection |
| **MPU6050** | I²C | 3-axis Accel, 3-axis Gyro | ±16g / ±2000°/s | Integrated motion & orientation |
| **BMP280** | I²C | Barometric pressure, Temperature | ~1 hPa / 0.01°C | Environmental context |
| **MAX30102** | I²C | Heart rate (optical PPG), SpO₂ | 16-bit | Reflective PPG sensor |
| **Load Cell** | ADC (with amp) | Force/pressure | ±10kg typical | Gesture/force input |

### Wearable Form Factor
- **MCU:** nRF52840 DK (development kit) → production form factor with coin battery
- **Power:** Currently USB-powered; ~500 mA at full sensor load
- **Connectivity:** BLE 5.0 (range ~100m line-of-sight)

---

## BLE Communication Protocol

### GATT Service Structure

```
Service: Custom Telemetry (UUID: TBD)
├─ Characteristic 1: IMU Data (UUID: TBD)
│  └─ Notify: CSV format "accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z"
├─ Characteristic 2: Heart Rate & SpO2 (UUID: TBD)
│  └─ Notify: CSV format "hr,spo2,temperature"
├─ Characteristic 3: EEG Signal (UUID: TBD)
│  └─ Notify: CSV format "eeg_raw"
├─ Characteristic 4: Pressure & Load (UUID: TBD)
│  └─ Notify: CSV format "pressure,load_cell"
└─ Characteristic 5: Device Status (UUID: TBD)
   └─ Read: JSON format {"battery_pct", "uptime_ms", "queue_depth"}
```

### BLE MTU & Fragmentation

- **MTU Size:** 247 bytes (maximum for nRF52840)
- **Packet Format:** CSV with newline terminator `\n`
- **Fragmentation Issue:** Initially, high-frequency IMU packets were split across multiple BLE writes, causing split frames on ESP32 side
  - **Solution:** Implement line-buffering on BLE client; reassemble complete lines before parsing
  - **Example:** "accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z\n" may arrive as: 
    - Write 1: "accel_x,accel_y,accel_z,gyro_x,gyro_y,"
    - Write 2: "gyro_z\n"

### BLE Client Implementation (ESP32 #1)

**Key features:**
- Non-blocking notification callbacks
- Ring buffer for received data
- Line-reassembly logic (handles MTU fragmentation)
- UART transmission to gateway ESP32

**Connection management:**
- Auto-reconnect on disconnect
- Exponential backoff: 100ms → 5s
- LED indicator: Blue = connected, Red = disconnected

---

## Firmware Sampling Rates

### nRF52840 Firmware Timings

| Sensor | Sampling Rate | Update Frequency | Rationale |
|--------|---------------|------------------|-----------|
| **EEG (ADC)** | 256 Hz | 256 samples/sec | Nyquist criterion for 50–128 Hz signals; captures alpha (8–12 Hz), beta (12–30 Hz), gamma (>30 Hz) bands |
| **IMU (Accel/Gyro)** | 100 Hz | 100 samples/sec | Sufficient for motion detection; balanced with BLE bandwidth |
| **ECG** | 128 Hz | 128 samples/sec | Nyquist for cardiac waveform (~40 Hz fundamental) |
| **Heart Rate (PPG)** | 100 Hz | 1 sample/sec (filtered) | MAX30102 reports final HR every 1 sec; internal sampling is continuous |
| **SpO₂** | 100 Hz raw | 1 sample/sec (filtered) | Reported after PPG processing |
| **Temperature** | 50 Hz raw | 1 sample/sec | Slow drift; oversample for noise rejection |
| **Pressure (BMP280)** | 25 Hz | 1 sample/sec | Environmental context; low-frequency change |
| **Load Cell** | 50 Hz | 50 samples/sec | Force gesture input |

### BLE Transmission Schedule

- **IMU packets:** Every 10 ms (100 Hz) → ~25 bytes CSV per packet
- **EEG packets:** Every 4 ms (256 Hz) → ~8 bytes CSV per packet  
- **Heart Rate:** Every 1000 ms (1 Hz) → ~10 bytes
- **Pressure/Temp:** Every 1000 ms (1 Hz) → ~10 bytes
- **Total BLE throughput:** ~350–400 bytes/sec (well within 1 Mbps nominal rate)

---

## Python Analysis Pipeline

### Server Architecture

**File:** `server.py`

```python
# WebSocket Server Structure
1. Listen on port 5000 (configurable)
2. Accept client connections (ESP32 gateway)
3. Stream telemetry data to all connected browsers (dashboard)
4. Buffer data for Python analysis modules
5. Real-time classification & band power computation
```

### Data Flow in Python

```
Incoming TCP/UART Stream (ESP32 Gateway)
           │
           ▼
   CSV Line Parser
   (regex-based split)
           │
           ▼
   Data Type Classifier
   (detect EEG, IMU, HR, etc.)
           │
    ┌──────┼──────┬──────┬────────┐
    ▼      ▼      ▼      ▼        ▼
  EEG   IMU    ECG    SpO2    Pressure
 Module  -      -      -        -
    │
    ▼
IIR Notch Filter (50 Hz)
    │
    ▼
Band Power Extraction
(Delta, Theta, Alpha, Beta, Gamma)
    │
    ▼
Random Forest Classifier
(Focused → Drowsy → Sleeping)
    │
    ▼
WebSocket Broadcast
(classification + band power)
```

### EEG Analysis Modules

#### 1. **Notch Filter (50 Hz Powerline Rejection)**

```python
# IIR Butterworth notch filter
# Parameters:
#   - Center frequency: 50 Hz
#   - Q factor: 30 (sharp roll-off)
#   - Sampling rate: 256 Hz
# Order: 2

Design: 
  Uses scipy.signal.iirnotch()
  Initialized once, applied in real-time streaming mode
```

**Performance:**
- Attenuation at 50 Hz: >40 dB
- Group delay: minimal (~2–3 samples)

#### 2. **Band Power Extraction**

Raw EEG → Apply notch filter → Compute power in frequency bands

**Frequency Bands:**

| Band | Frequency Range | Correlation with State |
|------|-----------------|----------------------|
| **Delta (δ)** | 0.5–4 Hz | Deep sleep, drowsiness |
| **Theta (θ)** | 4–8 Hz | Drowsiness, meditation |
| **Alpha (α)** | 8–12 Hz | Relaxation, eyes closed (baseline) |
| **Beta (β)** | 12–30 Hz | Alertness, mental activity |
| **Gamma (γ)** | 30–50 Hz | Cognitive load, focus; **blink-induced peaks** |

**Computation:**
- Welch's method (8-second windows, 50% overlap) for spectral density
- Power = integral of PSD over each band
- Log-scaled for feature input: `log(power + 1e-6)`

#### 3. **Random Forest Classifier**

**Target States:**
- Class 0: **Focused** (high beta, low delta/theta)
- Class 1: **Drowsy** (elevated theta, moderate delta, reduced beta)
- Class 2: **Sleeping** (high delta, very low beta)

**Training:**
- Single-channel frontal EEG (Fp1 placement from BioAmp Pill)
- Features: [Delta power, Theta power, Alpha power, Beta power, Gamma power] (5 features)
- Model: 100 trees, max depth 10, trained on labeled sessions

**Accuracy:** ~85% on test set (depends on individual calibration)

---

## EEG Signal Processing

### Video Demonstration Observations

**Demonstration:** Blink event causes high-frequency artifacts

```
Timeline:
─────────────────────────────────────────────────
  0 ms:     Eyes open (baseline)
           EEG: low gamma, normal alpha-beta mix
  
 100 ms:    Blink start
           EMG artifact in EEG (frontal muscles activate)
           Gamma power spikes (>30 Hz EMG leakage)
  
 150 ms:    Blink apex
           Peak artifact amplitude
  
 200 ms:    Blink end
           Artifact dissipates
  
 250 ms:    Return to baseline
           Gamma returns to normal
─────────────────────────────────────────────────
```

### Physical Mechanism

1. **Ocular Artifact (EOG):**
   - Eyelid movement generates ~50–100 µV surface potential
   - Spreads to frontal EEG channels via volume conduction

2. **Muscle Artifact (EMG):**
   - Orbicularis oculi contraction → high-frequency (>30 Hz) signal
   - Adds 20–200 µV spike during blink duration

3. **Combined Effect:**
   - Gamma band (30–50 Hz) shows dramatic peaks (can be 3–10× baseline)
   - Visible as bright "white noise" in spectrogram

### Mitigation Strategies

1. **Real-time notch filter:** Attenuates 50 Hz powerline; doesn't help blink artifact
2. **Band power monitoring:** Track gamma rise as **blink detector** (useful for alertness systems)
3. **Blind Source Separation (future):** ICA to separate EOG/EMG from neural EEG
4. **Reference electrode:** Future: bipolar montage (less artifact than monopolar from BioAmp Pill)

---

## Setup & Deployment

### Prerequisites

- **Hardware:**
  - nRF52840 DK (or Arduino-compatible nRF52840 board)
  - 2× ESP32 (any variant; WROOM or WROVER)
  - BioAmp EXG Pill (from Upside Down Labs)
  - MPU6050 / BMP280 / MAX30102 breakout boards
  - Load cell + amplifier (optional)
  - USB cables, dupont wires, breadboard

- **Software:**
  - Arduino IDE 1.8.19+ or PlatformIO
  - nRF5 SDK 17.1.0 (for nRF52840)
  - Python 3.8+

### nRF52840 Firmware Build

```bash
# Option 1: Arduino IDE
1. Install "Adafruit nRF52 Boards" package
2. Select "Adafruit Feather nRF52840 Express" (or compatible)
3. Load sketch from /wearables/nrf52840_firmware/
4. Upload

# Option 2: nRF5 SDK + arm-none-eabi-gcc
cd wearables/nrf52840_firmware/
make
```

**Key configuration files:**
- `config.h`: ADC sampling rates, sensor I²C addresses
- `ble_config.h`: MTU size, notification interval, UUID mappings

### ESP32 Client (BLE Relay)

**File:** `blercv-uart-trans/`

```bash
# Build with Arduino IDE
1. Install "ESP32 by Espressif" package
2. Load sketch
3. Configure UART pins:
   #define UART_TX_PIN 1  // GPIO 1 → nRF52840 RX
   #define UART_RX_PIN 3  // GPIO 3 ← nRF52840 TX
4. Upload

# Verify connection:
Serial monitor should show:
  > BLE scanning...
  > Found device: nRF52840-Wearable
  > Connected! MTU: 247
```

### ESP32 Gateway (WiFi/WebSocket)

**File:** `uartrcv-wifi/`

```bash
# Configure WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* serverIp = "YOUR_PYTHON_SERVER_IP";
const int serverPort = 5000;

# Build & upload
# Verify:
Serial monitor should show:
  > WiFi connecting...
  > Connected! IP: 192.168.x.x
  > Connecting to Python server...
  > Connected to server
```

### Python Server Setup

```bash
# Install dependencies
pip install -r requirements.txt
  # scipy, numpy, scikit-learn, websocket-server, flask

# Run server
python server.py --port 5000 --host 0.0.0.0

# Output:
  WebSocket server listening on 0.0.0.0:5000
  Waiting for client connections...
```

### Dashboard Deployment

**Files:** `dashboard.html` (main), `akshaydasboard.html` (variant)

```bash
# Option 1: Direct file open
Open dashboard.html in browser → connects to ws://localhost:5000

# Option 2: Serve with Flask
python server.py  # Automatically serves HTML at http://localhost:5000

# Configure server address in HTML:
const ws = new WebSocket("ws://YOUR_SERVER_IP:5000");
```

### Advanced EEG Setup (Single ESP32 & Real-Time FFT)

For dedicated EEG applications using the BioAmp EXG Pill, a streamlined single-ESP32 architecture is available. This removes the BLE requirement and directly samples the ADC while acting as a WiFi provisioning gateway and real-time DSP WebSocket server.

**Hardware Setup:**
- Connect the BioAmp EXG Pill output to the ESP32's `VN` pin (GPIO 39).
- Power the ESP32 via USB and the Pill from the ESP32's 3.3V pin.

**1. Firmware Provisioning (`esp32-vn-wifi-provisioning/esp32-vn-wifi-provisioning.ino`)**
- Flash the sketch to the ESP32.
- On first boot, it creates an `ESP32-Config` open WiFi hotspot. Connect to it (or visit `http://192.168.4.1`) to enter your local WiFi credentials and the laptop's IP address.
- **To Reset WiFi:** While the ESP32 is running normally, press and hold the **BOOT button (GPIO 0) for 3 seconds**. It will wipe its credentials, restart, and open the hotspot again.

**2. Python DSP Server (`eeg.py`)**
- Run `python3 eeg.py`. It will prompt you for a CSV filename to save the raw unfiltered logs.
- The server performs real-time 50 Hz Notch and 1-40 Hz Butterworth Bandpass filtering, calculating FFTs and Brainwave Bands (Alpha, Beta, etc.) on the fly.

**3. Real-Time Dashboard**
- Open `eeg_dashboard.html` in your browser to view the live filtered EEG waveforms, live FFT spectrum, and Focus Score bar charts.

---

## Project Structure

```
Wearable-IOT/
├── wearables/
│   └── nrf52840_firmware/          # nRF52840 embedded firmware
│       ├── main.ino                # Primary sketch
│       ├── ble_config.h            # GATT UUID mappings
│       ├── sensor_drivers/         # I²C & ADC drivers
│       │   ├── mpu6050.cpp
│       │   ├── bmp280.cpp
│       │   └── max30102.cpp
│       └── config.h                # Sampling rates, pins
│
├── blercv-uart-trans/              # ESP32 #1: BLE → UART relay
│   ├── main.ino                    # BLE client, UART transmitter
│   ├── ble_client.h                # BLE scanning & connection logic
│   └── uart_driver.h               # UART TX configuration
│
├── uartrcv-wifi/                   # ESP32 #2: WiFi gateway
│   ├── main.ino                    # UART RX, WiFi + WebSocket client
│   ├── wifi_config.h               # SSID, server IP, port
│   └── freertos_queue_mgmt.h       # Queue overflow handling
│
├── server.py                       # Python WebSocket listener
│   ├── eeg_analysis/               # EEG signal processing
│   │   ├── notch_filter.py         # 50 Hz IIR notch
│   │   ├── band_power.py           # Welch PSD + band extraction
│   │   └── classifier.py           # Random Forest model
│   ├── data_parser.py              # CSV line parser
│   └── websocket_relay.py          # Client/browser broadcast
│
├── dashboard.html                  # Real-time web visualization
│   ├── Plots: EEG waveform, band power, spectrum
│   ├── Alerts: Drowsiness warning, state classification
│   └── Status: BLE/WiFi/Server connection indicators
│
├── trial.py                        # Experimental analysis scripts
└── README.md                       # This file
```

---

## Key Design Patterns

### 1. FreeRTOS Queue Management (ESP32 Gateway)

```cpp
// Prevents data loss during WiFi reconnection
QueueHandle_t telemetry_queue = xQueueCreate(1024, sizeof(char) * 128);

// Producer (UART ISR)
BaseType_t xHigherPriorityTaskWoken;
xQueueSendFromISR(telemetry_queue, buffer, &xHigherPriorityTaskWoken);

// Consumer (WebSocket task)
while (true) {
    if (xQueueReceive(telemetry_queue, buffer, pdMS_TO_TICKS(100))) {
        ws_client.send(buffer);
    }
    // If WiFi down, queue fills up; older data discarded (FIFO overflow)
    // Recovery: auto-reconnect, resume transmission
}
```

### 2. Heartbeat for Connection Health

```cpp
// ESP32 gateway periodically sends heartbeat
unsigned long last_heartbeat = millis();
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 seconds

if (millis() - last_heartbeat > HEARTBEAT_INTERVAL) {
    ws_client.send("{\"type\": \"heartbeat\"}");
    last_heartbeat = millis();
}

// Python server detects stale connections
# If no heartbeat for 15 seconds, mark connection dead
```

### 3. LED Status Indicators

| LED | Color | State |
|-----|-------|-------|
| **1** | Green | System running |
| **2** | Blue | BLE connected |
| **3** | Red | BLE disconnected |
| **1** | Blinking | WiFi connecting |
| **2** | Blinking | WebSocket connecting |

---

## Performance Metrics

### Data Throughput

| Component | Data Rate | Latency |
|-----------|-----------|---------|
| nRF52840 → ESP32 (BLE) | ~380 bytes/sec | 10–50 ms |
| ESP32 → Python (WiFi/WebSocket) | ~380 bytes/sec | 50–200 ms |
| Python → Dashboard (WebSocket) | ~380 bytes/sec | 100–300 ms |
| **End-to-End** | — | **200–500 ms** |

### Power Consumption (preliminary)

| State | Current |
|-------|---------|
| nRF52840 idle | ~2 µA |
| nRF52840 BLE active | ~15 mA |
| nRF52840 all sensors + BLE | ~40 mA |
| ESP32 WiFi + BLE | ~100 mA |

---

## Troubleshooting

### BLE Disconnections

**Symptom:** ESP32 client repeatedly reconnects to nRF52840

**Causes & Solutions:**
1. **MTU size mismatch:** Ensure both sides agree on 247 bytes
   - Check nRF5 SDK log: `NRF_LOG_INFO("MTU negotiated: %d", mtu)`
2. **Power supply instability:** Use proper 5V regulator, not USB port
3. **Antenna placement:** nRF52840 antenna clear of metal objects

### WiFi Dropouts

**Symptom:** ESP32 gateway loses WiFi every few minutes

**Solutions:**
1. Check 2.4 GHz interference (microwave, Bluetooth devices)
2. Increase reconnection timeout: `WiFi.setAutoReconnect(true)`
3. Add WiFi.mode(WIFI_STA) to disable AP mode

### EEG Signal Quality

**Symptom:** Band power values are all zero or NaN

**Causes:**
1. **ADC saturation:** Reduce analog gain in BioAmp Pill
2. **Noisy power supply:** Use shielded cables, separate analog/digital grounds
3. **Electrode contact:** Ensure good skin contact; moisten with electrolyte gel

### Python Server Crashes

**Symptom:** Server stops accepting connections after a few hours

**Solutions:**
1. Add memory profiling: `memory_profiler` module
2. Limit ring buffer size; implement circular queue
3. Check WebSocket client count: `len(self.clients)`

---

## Future Enhancements

- [ ] **Binary packet format:** Replace CSV with protobuf for 5× bandwidth reduction
- [ ] **Edge ML:** Deploy RandomForest classifier directly on nRF52840; reduce latency
- [ ] **Multi-channel EEG:** Integrate 8-channel EEG cap for spatial filtering & ICA
- [ ] **SD card logging:** On-device data storage for post-hoc analysis
- [ ] **Battery management:** Implement power-gating, dynamic sampling rate adjustment
- [ ] **Mobile app:** Flutter/React Native companion app instead of web dashboard
- [ ] **Cloud sync:** Optional AWS/GCP integration for long-term analytics
- [ ] **Personalization:** Per-user classifier training; account for individual EEG baseline variation

---

## References & Resources

- **BioAmp Exg Pill:** https://upsidedownlabs.com/
- **nRF52840 SDK:** https://github.com/NordicSemiconductor/nRF5_SDK
- **ESP32 Arduino:** https://github.com/espressif/arduino-esp32
- **EEG Signal Processing:** Teplan M. "Fundamentals of EEG Measurement" (2002)
- **Drowsiness Detection:** Jiao et al. "EEG-based Drowsiness Detection using CNN" (IEEE, 2018)

---

## License

This project is licensed under the **MIT License**. See LICENSE file for details.

---

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/YourFeature`)
3. Commit changes (`git commit -m "Add YourFeature"`)
4. Push to branch (`git push origin feature/YourFeature`)
5. Open a Pull Request

---

## Contact

**Lakshya** | Electronics & Telecom Engineering, VJTI Mumbai  
GitHub: [@Lakshyaa1](https://github.com/Lakshyaa1)

---

## Acknowledgments

- **Upside Down Labs** for the BioAmp EXG Pill & open-source signal processing guides
- **Nordic Semiconductor** for nRF5 SDK & excellent documentation
- **SRA-VJTI** mentorship & robotics experience that informed this architecture
- **KDAIL internship** experience with sensor integration & real-time systems