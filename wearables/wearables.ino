#include <bluefruit.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include "HX711.h"
#include "MAX30105.h"

Adafruit_FlashTransport_QSPI flashTransport;

// =====================
// TIMING INTERVALS
// =====================
constexpr uint32_t INTERVAL_EEG_US = 10000;       // 100 Hz
constexpr uint32_t INTERVAL_ECG_MS = 20;          // 50 Hz
constexpr uint32_t INTERVAL_10HZ_MS = 100;        // 10 Hz
constexpr uint32_t INTERVAL_RECONNECT_MS = 10000; // 10 seconds

// =====================
// EEG CONFIG & STRUCT
// =====================
#define EEG_PIN A3
constexpr uint8_t BATCH_SIZE = 10;

#pragma pack(push,1)
struct EEGPacket {
  uint16_t samples[BATCH_SIZE];
};
#pragma pack(pop)

EEGPacket eegPacket;
uint8_t batchIndex = 0;

// =====================
// SENSOR STRUCTS & GLOBALS
// =====================
struct SensorData {
  long Red;
  long IR;
};

struct IMUData {
  float aX, aY, aZ;
  float gX, gY, gZ;
};

LSM6DS3 myIMU(I2C_MODE, 0x6A);

#define DATA_PIN 7
#define CLOCK_PIN 8
HX711 scale;

MAX30105 particleSensor;

bool deviceConnected = false;

// Sensor Status Flags
bool imuReady = false;
bool spo2Ready = false;

// Timers
uint32_t lastEegUs = 0;
uint32_t lastEcgMs = 0;
uint32_t last10HzMs = 0;
uint32_t lastReconnectMs = 0;

// =====================
// BLE GATT Profile
// =====================
BLEService customService = BLEService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
BLECharacteristic imuChar = BLECharacteristic(0x27A8);
BLECharacteristic ecgChar = BLECharacteristic(0x2A37);
BLECharacteristic loadChar = BLECharacteristic(0x2A98);
BLECharacteristic spo2Char = BLECharacteristic(0x2A8D);
BLECharacteristic eegChar = BLECharacteristic(0x2A99); 

// =====================
// SENSOR INITS
// =====================
void initSPO2() {
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("SPO2 not found. Will retry later.");
    spo2Ready = false;
  } else {
    byte ledBrightness = 40;
    byte sampleAverage = 1;
    byte ledMode = 2;
    int sampleRate = 3200;
    int pulseWidth = 69;
    int adcRange = 4096;
    particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
    Serial.println("SPO2 OK");
    spo2Ready = true;
  }
}

void initIMU() {
  Wire.setClock(400000); 
  if (myIMU.begin() != 0) {
    Serial.println("IMU not found. Will retry later.");
    imuReady = false;
  } else {
    Serial.println("IMU OK");
    imuReady = true;
  }
}

void setupNRF(void) {
  customService.begin();

  imuChar.setProperties(CHR_PROPS_NOTIFY);
  imuChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  imuChar.setFixedLen(64);
  imuChar.begin();

  ecgChar.setProperties(CHR_PROPS_NOTIFY);
  ecgChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  ecgChar.begin();

  loadChar.setProperties(CHR_PROPS_NOTIFY);
  loadChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  loadChar.begin();

  spo2Char.setProperties(CHR_PROPS_NOTIFY);
  spo2Char.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  spo2Char.begin();

  eegChar.setProperties(CHR_PROPS_NOTIFY);
  eegChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  eegChar.setFixedLen(sizeof(EEGPacket));
  eegChar.begin();
}

void startAdv() {
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(customService); 
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 1600);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

void connect_callback(uint16_t conn_handle) {
  deviceConnected = true;
  Serial.println("BLE Connected!");
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  deviceConnected = false;
  Serial.println("BLE Disconnected!");
}

void setup() {
  Serial.begin(115200);
  
  // FIX: Give Serial up to 5 seconds to wake up so you don't miss prints
  unsigned long start = millis();
  while (!Serial && (millis() - start < 5000)) {
    delay(10);
  }
  
  Serial.println("\n--- IN SETUP ---");

  NRF_POWER->DCDCEN = 1;

  flashTransport.begin();
  flashTransport.runCommand(0xB9);
  delayMicroseconds(5);
  flashTransport.end();

  Wire.begin();

  // Pins
  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(EEG_PIN, INPUT);

  // Loadcell init (No blocking tare)
  scale.begin(DATA_PIN, CLOCK_PIN);
  scale.set_scale(121.16);
  Serial.println("Loadcell setup executed (tare skipped)");

  initSPO2();
  initIMU(); 

  // BLE init
  Bluefruit.begin();
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.setName("Wearables");
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  setupNRF();
  startAdv();

  Serial.println("Setup complete! BLE Advertising started.");
  
  lastEegUs = micros();
  lastEcgMs = millis();
  last10HzMs = millis();
  lastReconnectMs = millis();
}

// =====================
// DATA READERS
// =====================
IMUData readIMUData() {
  IMUData data = {0,0,0,0,0,0};
  if (!imuReady) return data;
  data.aX = myIMU.readFloatAccelX();
  data.aY = myIMU.readFloatAccelY();
  data.aZ = myIMU.readFloatAccelZ();
  data.gX = myIMU.readFloatGyroX();
  data.gY = myIMU.readFloatGyroY();
  data.gZ = myIMU.readFloatGyroZ();
  return data;
}

int updateECG() {
  if (digitalRead(A1) == HIGH || digitalRead(A2) == HIGH) {
    return 0; // Leads off
  } else {
    return analogRead(A0);
  }
}

float updateLoadcell() {
  if (scale.is_ready()) {
    return scale.get_units();
  }
  return 0.0;
}

SensorData readRedIR() {
  SensorData data = {0,0};
  if (!spo2Ready) return data;

  Wire.beginTransmission(0x57);
  Wire.write(0x07);
  Wire.endTransmission();
  Wire.requestFrom(0x57, 6, true);

  if(Wire.available() >= 6) {
    data.Red = (Wire.read() << 16 | Wire.read() << 8 | Wire.read()) & 0x3FFFF;
    data.IR = (Wire.read() << 16 | Wire.read() << 8 | Wire.read()) & 0x3FFFF;
  }
  return data;
}

// =====================
// MAIN LOOP
// =====================
void loop() {
  uint32_t nowUs = micros();
  uint32_t nowMs = millis();

  // FIX: Heartbeat if disconnected so you know it's alive and waiting
  static uint32_t lastHeartbeat = 0;
  if (!Bluefruit.connected() && (nowMs - lastHeartbeat >= 2000)) {
    lastHeartbeat = nowMs;
    Serial.println("Advertising... Waiting for BLE connection.");
  }

  // --- RECONNECT LOGIC (Every 10 Seconds) ---
  if (nowMs - lastReconnectMs >= INTERVAL_RECONNECT_MS) {
    lastReconnectMs = nowMs;
    
    if (!spo2Ready) {
      Serial.println("Attempting SPO2 Reconnect...");
      initSPO2();
    }
    
    if (!imuReady) {
      Serial.println("Attempting IMU Reconnect...");
      initIMU();
    }
  }

  // ONLY TRANSMIT SENSOR DATA IF CONNECTED
  if (Bluefruit.connected()) {
    
    // --- 1. EEG @ 100Hz ---
    if ((nowUs - lastEegUs) >= INTERVAL_EEG_US) {
      lastEegUs += INTERVAL_EEG_US; 

      eegPacket.samples[batchIndex] = analogRead(EEG_PIN);
      batchIndex++;

      if (batchIndex >= BATCH_SIZE) {
        eegChar.notify((uint8_t*)&eegPacket, sizeof(EEGPacket));
        batchIndex = 0;
      }
    }

    // --- 2. ECG @ 50Hz ---
    if (nowMs - lastEcgMs >= INTERVAL_ECG_MS) {
      lastEcgMs += INTERVAL_ECG_MS;
      
      int ecgValue = updateECG();
      char ecgStr[16];
      snprintf(ecgStr, sizeof(ecgStr), "%d", ecgValue);
      ecgChar.notify((uint8_t*)ecgStr, strlen(ecgStr));
    }

    // --- 3. OTHER SENSORS @ 10Hz ---
    if (nowMs - last10HzMs >= INTERVAL_10HZ_MS) {
      last10HzMs += INTERVAL_10HZ_MS;

      // IMU
      if (imuReady) {
        IMUData imu = readIMUData();
        char imuStr[80];
        snprintf(imuStr, sizeof(imuStr),
                "AX:%.2f AY:%.2f AZ:%.2f GX:%.1f GY:%.1f GZ:%.1f",
                imu.aX, imu.aY, imu.aZ, imu.gX, imu.gY, imu.gZ);
        imuChar.notify((uint8_t*)imuStr, strlen(imuStr));
      }

      // Loadcell
      float weight = updateLoadcell();
      char loadStr[16];
      snprintf(loadStr, sizeof(loadStr), "%.2f", weight);
      loadChar.notify((uint8_t*)loadStr, strlen(loadStr));

      // SPO2
      if (spo2Ready) {
        SensorData data = readRedIR();
        char dataStr[40];
        snprintf(dataStr, sizeof(dataStr), "%ld,%ld", data.Red, data.IR);
        spo2Char.notify((uint8_t*)dataStr, strlen(dataStr));
      }
    }
  }
}