#include "BLEDevice.h"

#define ONBOARD_LED 2

// ===============================
// EEG STRUCT
// ===============================
#pragma pack(push,1)
struct EEGPacket {
    uint16_t samples[10];
};
#pragma pack(pop)

// ===============================
// UUID DEFINITIONS
// ===============================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID_imu((uint16_t)0x27A8);
static BLEUUID charUUID_ecg((uint16_t)0x2A37);
static BLEUUID charUUID_load((uint16_t)0x2A98);
static BLEUUID charUUID_spo2((uint16_t)0x2A8D);
static BLEUUID charUUID_eeg((uint16_t)0x2A99);  

static BLEAdvertisedDevice* myDevice;
static BLERemoteCharacteristic *imuChar, *ecgChar, *loadChar, *spo2Char, *eegChar;

bool doConnect = false, connected = false, doScan = true;

// ===============================
// GLOBAL COUNTERS
// ===============================
uint32_t eegPackets = 0, eegSamples = 0, imuPackets = 0, ecgPackets = 0, loadPackets = 0, spo2Packets = 0;
uint32_t totalBlePackets = 0; // The missing link for BLE input rate
uint32_t uartMessages = 0, uartBytes = 0, lastStats = 0;uint32_t uartEEGPackets = 0; 

// ===============================
// PARSER FUNCTION
// ===============================
void parseMessage(uint8_t type_, uint8_t* pData, size_t length) {
  char message[512]; 
  int idx = 0;
  message[0] = '\0';

  if (type_ == 5) { // EEG
    if (length != sizeof(EEGPacket)) return;
    EEGPacket packet;
    memcpy(&packet, pData, sizeof(packet));
    eegPackets++; eegSamples += 10;
    idx += sprintf(message, "{\"dev\":\"EEG\",\"samples\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}", 
                   packet.samples[0], packet.samples[1], packet.samples[2], packet.samples[3], 
                   packet.samples[4], packet.samples[5], packet.samples[6], packet.samples[7], 
                   packet.samples[8], packet.samples[9]);
  } else {
    String dataStr = "";
    for (size_t i = 0; i < length; i++) dataStr += (char)pData[i];
    idx += sprintf(message + idx, "{");
    if (type_ == 1) { 
        imuPackets++;
        float ax, ay, az, gx, gy, gz;
        if (sscanf(dataStr.c_str(), "AX:%f AY:%f AZ:%f GX:%f GY:%f GZ:%f", &ax, &ay, &az, &gx, &gy, &gz) == 6)
            idx += sprintf(message + idx, "\"dev\":\"IMU\",\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f", ax, ay, az, gx, gy, gz);
        else idx += sprintf(message + idx, "\"dev\":\"IMU\",\"raw\":\"%s\"", dataStr.c_str());
    } 
    else if (type_ == 2) { ecgPackets++; idx += sprintf(message + idx, "\"dev\":\"ECG\",\"ecg\":%d", atoi(dataStr.c_str())); }
    else if (type_ == 3) { loadPackets++; idx += sprintf(message + idx, "\"dev\":\"Load\",\"load\":%.2f", atof(dataStr.c_str())); }
    else if (type_ == 4) { spo2Packets++; long red, ir; if (sscanf(dataStr.c_str(), "%ld,%ld", &red, &ir) == 2) idx += sprintf(message + idx, "\"dev\":\"RedIR\",\"Red\":%ld,\"IR\":%ld", red, ir); }
    idx += sprintf(message + idx, "}");
  }

  Serial1.println(message);
  uartMessages++;
  uartBytes += strlen(message) + 2;
  if (type_ == 5)
    uartEEGPackets++;
 }

// ===============================
// BLE CALLBACKS
// ===============================
void notifyCallback(BLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  // CRITICAL: Increment total BLE packets every time a notify hits
  totalBlePackets++;
  
  BLEUUID uuid = characteristic->getUUID();
  uint8_t type_ = 0;
  if (uuid.equals(charUUID_imu)) type_ = 1;
  else if (uuid.equals(charUUID_ecg)) type_ = 2;
  else if (uuid.equals(charUUID_load)) type_ = 3;
  else if (uuid.equals(charUUID_spo2)) type_ = 4;
  else if (uuid.equals(charUUID_eeg)) type_ = 5;
  if (type_ > 0) parseMessage(type_, data, length);
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) { Serial.println("[BLE] Connected"); digitalWrite(ONBOARD_LED, HIGH); }
  void onDisconnect(BLEClient* pClient) { connected = false; doScan = true; Serial.println("[BLE] Disconnected"); digitalWrite(ONBOARD_LED, LOW); }
};

bool connectToServer() {
  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  pClient->connect(myDevice);
  BLERemoteService* service = pClient->getService(serviceUUID);
  if (!service) { pClient->disconnect(); return false; }
  
  imuChar = service->getCharacteristic(charUUID_imu);
  ecgChar = service->getCharacteristic(charUUID_ecg);
  loadChar = service->getCharacteristic(charUUID_load);
  spo2Char = service->getCharacteristic(charUUID_spo2);
  eegChar = service->getCharacteristic(charUUID_eeg);
  
  if (imuChar && imuChar->canNotify()) imuChar->registerForNotify(notifyCallback);
  if (ecgChar && ecgChar->canNotify()) ecgChar->registerForNotify(notifyCallback);
  if (loadChar && loadChar->canNotify()) loadChar->registerForNotify(notifyCallback);
  if (spo2Char && spo2Char->canNotify()) spo2Char->registerForNotify(notifyCallback);
  if (eegChar && eegChar->canNotify()) eegChar->registerForNotify(notifyCallback);
  
  return connected = true;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice adv) {
    if (adv.getName() == "Wearables") { BLEDevice::getScan()->stop(); myDevice = new BLEAdvertisedDevice(adv); doConnect = true; doScan = false; }
  }
};

void setup() {
  Serial.begin(115200); Serial1.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(ONBOARD_LED, OUTPUT);
  BLEDevice::init("Wearables_Client");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  scan->setActiveScan(true);
  scan->start(5, false);
}

void loop() {
  if (doConnect) { if (connectToServer()) Serial.println("[MAIN] Connected!"); else doScan = true; doConnect = false; }
  if (!connected && doScan) BLEDevice::getScan()->start(5, false);

  if (millis() - lastStats >= 5000) {
      Serial.println("\n╔════════════════════════════════════════╗");
      Serial.println("║        BLE & UART STATUS (5s)          ║");
      Serial.println("╠════════════════════════════════════════╣");
      Serial.printf("║ BLE-EEG Pkts: %-4lu | Samples: %-6lu  ║\n", eegPackets, eegSamples);
      Serial.printf("║ BLE-IMU Pkts: %-4lu | BLE-ECG Pkts: %-4lu  ║\n", imuPackets, ecgPackets);
      Serial.printf("║ BLE-Ld  Pkts: %-4lu | BLE-SpO2 Pkts:%-4lu  ║\n", loadPackets, spo2Packets);
      Serial.println("╠════════════════════════════════════════╣");
      // Now you can directly compare Input vs Output rates
      Serial.printf("║ BLE EEG Rate:      %-4lu pkts/sec      ║\n", eegPackets / 5);
Serial.printf("║ UART EEG Rate:     %-4lu pkts/sec      ║\n", uartEEGPackets / 5);

Serial.printf("║ Total BLE In:      %-4lu pkts/sec      ║\n", totalBlePackets / 5);
Serial.printf("║ UART Tx Total:     %-4lu msg/sec       ║\n", uartMessages / 5);
Serial.printf("║ UART Tx Band:      %-5lu bytes/sec     ║\n", uartBytes / 5);
      Serial.println("╚════════════════════════════════════════╝");

      eegPackets = eegSamples = imuPackets = ecgPackets = loadPackets = spo2Packets = 0;
      totalBlePackets = 0;
      uartMessages = uartBytes = 0;
      uartEEGPackets = 0;
      lastStats = millis();
  }
  delay(10);
}