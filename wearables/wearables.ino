#include <bluefruit.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include "HX711.h"
#include "MAX30105.h"

Adafruit_FlashTransport_QSPI flashTransport;

// Struct to hold Red and IR values
struct SensorData {
  long Red;
  long IR;
};

// IMU
struct IMUData {
  float aX, aY, aZ;
  float gX, gY, gZ;
};

LSM6DS3 myIMU(I2C_MODE, 0x6A);

// LOADCELL
#define DATA_PIN 7
#define CLOCK_PIN 8
HX711 scale;

// EEG
#define EEG_PIN A3

// SPO2
MAX30105 particleSensor;
uint16_t Red;
uint16_t IR;

// Timing & Sampling
unsigned long nowMicros = 0;
unsigned long lastMicros = 0;
const int MAX_SAMPLING_FREQ = 100;
unsigned long MINIMUM_SAMPLING_DELAY_uSec = (unsigned long)(1 * 1000000 / MAX_SAMPLING_FREQ);
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 20; 

// --- SENSOR AVAILABILITY FLAGS ---
bool imuAvailable = false;
bool loadcellAvailable = false;
bool max30105Available = false;
unsigned long lastReconnectAttempt = 0;

bool deviceConnected = false;

////BLE GATT Profile////
BLEService customService = BLEService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
BLECharacteristic imuChar = BLECharacteristic(0x27A8);
BLECharacteristic ecgChar = BLECharacteristic(0x2A37);
BLECharacteristic loadChar = BLECharacteristic(0x2A98);
BLECharacteristic spo2Char = BLECharacteristic(0x2A8D);
BLECharacteristic eegChar = BLECharacteristic(0x2A99);


void setup() {
  Serial.begin(115200);

  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  Serial.println("[SYSTEM] Booting...");

  NRF_POWER->DCDCEN = 1;

  flashTransport.begin();
  flashTransport.runCommand(0xB9);
  flashTransport.end();

  Wire.begin();

  // --- NON-BLOCKING LOADCELL INIT ---
  scale.begin(DATA_PIN, CLOCK_PIN);
  if (scale.is_ready()) {
    scale.set_scale(121.16);
    // Tare removed intentionally to prevent startup freezing or bad calibration
    loadcellAvailable = true;
    Serial.println("[LOADCELL] Init OK");
  } else {
    loadcellAvailable = false;
    Serial.println("[LOADCELL] Not detected");
  }

  // --- NON-BLOCKING MAX30105 INIT ---
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    max30105Available = false;
    Serial.println("[MAX30105] Not detected");
  } else {
    max30105Available = true;
    Serial.println("[MAX30105] Init OK");
    particleSensor.setup(40, 1, 2, 3200, 69, 4096);
  }

  Wire.setClock(400000);

  // --- NON-BLOCKING IMU INIT ---
  if (myIMU.begin() != 0) {
    imuAvailable = false;
    Serial.println("[IMU] Not detected");
  } else {
    imuAvailable = true;
    Serial.println("[IMU] Init OK");
  }

  Bluefruit.begin();
  Bluefruit.setName("Wearables");
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  setupNRF();
  startAdv();

  Serial.println("[SYSTEM] Setup complete. Running loop...");
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
  eegChar.begin();
}

void startAdv() {
  Serial.println("[BLE] Starting advertisement...");
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 1600);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

IMUData readIMUData() {
  IMUData data;
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

float updateEEG() {
  float sensor_value = analogRead(EEG_PIN);
  return EEGFilter(sensor_value);
}

float updateLoadcell() {
  if (scale.is_ready()) {
    return scale.get_units();
  } else {
    return 0;
  }
}

SensorData readRedIR() {
  SensorData data = {0, 0};
  Wire.beginTransmission(0x57);
  Wire.write(0x07);
  Wire.endTransmission();
  Wire.requestFrom(0x57, 6, true);

  data.Red = (Wire.read() << 16 | Wire.read() << 8 | Wire.read()) & 0x3FFFF;
  data.IR = (Wire.read() << 16 | Wire.read() << 8 | Wire.read()) & 0x3FFFF;
  return data;
}

float EEGFilter(float input) {
  float output = input;
  {
    static float z1, z2;
    float x = output - -0.95391350*z1 - 0.25311356*z2;
    output = 0.00735282*x + 0.01470564*z1 + 0.00735282*z2;
    z2 = z1; z1 = x;
  }
  {
    static float z1, z2;
    float x = output - -1.20596630*z1 - 0.60558332*z2;
    output = 1.00000000*x + 2.00000000*z1 + 1.00000000*z2;
    z2 = z1; z1 = x;
  }
  {
    static float z1, z2;
    float x = output - -1.97690645*z1 - 0.97706395*z2;
    output = 1.00000000*x + -2.00000000*z1 + 1.00000000*z2;
    z2 = z1; z1 = x;
  }
  {
    static float z1, z2;
    float x = output - -1.99071687*z1 - 0.99086813*z2;
    output = 1.00000000*x + -2.00000000*z1 + 1.00000000*z2;
    z2 = z1; z1 = x;
  }
  return output;
}

void loop() {
  unsigned long currentTime = millis();

  // --- RECONNECTION LOGIC (Every 5 seconds) ---
  if (currentTime - lastReconnectAttempt > 5000) {
    lastReconnectAttempt = currentTime;

    if (!loadcellAvailable) {
      if (scale.is_ready()) {
        scale.set_scale(121.16);
        loadcellAvailable = true;
        Serial.println("[LOADCELL] Reconnected");
      }
    }

    if (!imuAvailable) {
      if (myIMU.begin() == 0) {
        imuAvailable = true;
        Serial.println("[IMU] Reconnected");
      }
    }

    if (!max30105Available) {
      if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
        particleSensor.setup(40, 1, 2, 3200, 69, 4096);
        max30105Available = true;
        Serial.println("[MAX30105] Reconnected");
      }
    }
  }

  // --- GUARDED SENSOR READINGS ---
  IMUData imu = {0};
  if (imuAvailable) {
    imu = readIMUData();
  }

  float weight = 0;
  if (loadcellAvailable) {
    weight = updateLoadcell();
  }

  SensorData spo2Data = {0};
  if (max30105Available) {
    spo2Data = readRedIR();
  }

  // Analog reads (ECG/EEG) don't lock I2C, so they are naturally safe to read anytime
  int ecgValue = updateECG();
  float eegValue = updateEEG();

  // === SERIAL OUTPUT ===
  Serial.print("[");
  Serial.print(currentTime);
  Serial.print("] [SENSORS] ");
  Serial.print("IMU(A): ");
  Serial.print(imu.aX, 2); Serial.print(",");
  Serial.print(imu.aY, 2); Serial.print(",");
  Serial.print(imu.aZ, 2);
  Serial.print(" (G): ");
  Serial.print(imu.gX, 1); Serial.print(",");
  Serial.print(imu.gY, 1); Serial.print(",");
  Serial.print(imu.gZ, 1);
  Serial.print(" | ECG: "); Serial.print(ecgValue);
  Serial.print(" | EEG: "); Serial.print(eegValue, 2);
  Serial.print(" | Load: "); Serial.print(weight, 2); Serial.print("kg");
  Serial.print(" | SPO2(R,IR): "); Serial.print(spo2Data.Red); Serial.print(",");
  Serial.println(spo2Data.IR);

  // === BLE TRANSMISSION ===
  if (Bluefruit.connected()) {
    char imuStr[80];
    snprintf(imuStr, sizeof(imuStr),
            "AX:%.2f AY:%.2f AZ:%.2f GX:%.1f GY:%.1f GZ:%.1f",
            imu.aX, imu.aY, imu.aZ, imu.gX, imu.gY, imu.gZ);
    imuChar.notify((uint8_t*)imuStr, strlen(imuStr));

    char ecgStr[16];
    snprintf(ecgStr, sizeof(ecgStr), "%d", ecgValue);
    ecgChar.notify((uint8_t*)ecgStr, strlen(ecgStr));

    char eegStr[20];
    snprintf(eegStr, sizeof(eegStr), "%.2f", eegValue);
    eegChar.notify((uint8_t*)eegStr, strlen(eegStr));

    char loadStr[16];
    snprintf(loadStr, sizeof(loadStr), "%.2f", weight);
    loadChar.notify((uint8_t*)loadStr, strlen(loadStr));

    char dataStr[40];
    snprintf(dataStr, sizeof(dataStr), "%ld,%ld", spo2Data.Red, spo2Data.IR);
    spo2Char.notify((uint8_t*)dataStr, strlen(dataStr));
  }

  delay(50);
}

void connect_callback(uint16_t conn_handle) {
  deviceConnected = true;
  Serial.println("\n[BLE] *** CONNECTED ***\n");
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  deviceConnected = false;
  Serial.println("\n[BLE] *** DISCONNECTED (resuming BLE advertising) ***\n");
}