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
static BLERemoteCharacteristic* imuChar;
static BLERemoteCharacteristic* ecgChar;
static BLERemoteCharacteristic* loadChar;
static BLERemoteCharacteristic* spo2Char;
static BLERemoteCharacteristic* eegChar;  

bool doConnect = false;
bool connected = false;
bool doScan = true;

// ===============================
// GLOBAL COUNTERS & STATS
// ===============================
uint32_t eegPackets = 0;
uint32_t eegSamples = 0;
uint32_t imuPackets = 0;
uint32_t ecgPackets = 0;
uint32_t loadPackets = 0;
uint32_t spo2Packets = 0;

uint32_t lastStats = 0;

// ===============================
// PARSER FUNCTION
// ===============================
void parseMessage(uint8_t type_, uint8_t* pData, size_t length) {
  char message[512]; // Generous buffer for JSON
  int idx = 0;
  message[0] = '\0';

  // --- TYPE 5: EEG (Binary Batch) ---
  if (type_ == 5) {
    if (length != sizeof(EEGPacket)) {
      Serial.print("Bad EEG packet length: ");
      Serial.println(length);
      return;
    }
    EEGPacket packet;
    memcpy(&packet, pData, sizeof(packet));
    
    eegPackets++;
    eegSamples += 10;
    
    idx += sprintf(message + idx, "{\"dev\":\"EEG\",\"samples\":[");
    for(int i = 0; i < 10; i++) {
      idx += sprintf(message + idx, "%u", packet.samples[i]);
      if(i < 9) idx += sprintf(message + idx, ",");
    }
    idx += sprintf(message + idx, "]}");
  } 
  // --- TYPES 1-4: Standard Sensors (String Parsing) ---
  else {
    String dataStr = "";
    for (size_t i = 0; i < length; i++) dataStr += (char)pData[i];
    idx += sprintf(message + idx, "{");
    
    if (type_ == 1) { 
      // IMU data
      imuPackets++;
      float ax, ay, az, gx, gy, gz;
      int count = sscanf(dataStr.c_str(), "AX:%f AY:%f AZ:%f GX:%f GY:%f GZ:%f", &ax, &ay, &az, &gx, &gy, &gz);
      if (count == 6) {
        idx += sprintf(message + idx, "\"dev\":\"IMU\",\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f", ax, ay, az, gx, gy, gz);
      } else {
        idx += sprintf(message + idx, "\"dev\":\"IMU\",\"raw\":\"%s\"", dataStr.c_str());
      }
    } 
    else if (type_ == 2) { 
      // ECG data
      ecgPackets++;
      int ecgVal = atoi(dataStr.c_str());
      idx += sprintf(message + idx, "\"dev\":\"ECG\",\"ecg\":%d", ecgVal);
    } 
    else if (type_ == 3) { 
      // Loadcell data
      loadPackets++;
      float load = atof(dataStr.c_str());
      idx += sprintf(message + idx, "\"dev\":\"Load\",\"load\":%.2f", load);
    }
    else if (type_ == 4) {
      // SPO2 data
      spo2Packets++;
      long red, ir;
      int count = sscanf(dataStr.c_str(), "%ld,%ld", &red, &ir);
      if (count == 2) {
        idx += sprintf(message + idx, "\"dev\":\"RedIR\",\"Red\":%ld,\"IR\":%ld", red, ir);
      }
    }
    idx += sprintf(message + idx, "}");
  }

  // Print JSON to Hardware UART only to save USB CPU time
  // Serial.println(message); 
  Serial1.println(message);
  Serial1.flush(); // Ensure complete UART transmission
}

// ===============================
// NOTIFICATION CALLBACK
// ===============================
void notifyCallback(BLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  BLEUUID uuid = characteristic->getUUID();
  uint8_t type_ = 0;
  if (uuid.equals(charUUID_imu)) type_ = 1;
  else if (uuid.equals(charUUID_ecg)) type_ = 2;
  else if (uuid.equals(charUUID_load)) type_ = 3;
  else if (uuid.equals(charUUID_spo2)) type_ = 4;
  else if (uuid.equals(charUUID_eeg)) type_ = 5;

  if (type_ > 0) {
    parseMessage(type_, data, length);
  } else {
    Serial.println("Unknown characteristic notification received!");
  }
}

// ===============================
// CLIENT CALLBACKS
// ===============================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) {
    Serial.println("[BLE] Connected to server");
    digitalWrite(ONBOARD_LED, HIGH);
  }
  void onDisconnect(BLEClient* pClient) {
    connected = false;
    doScan = true;
    Serial.println("[BLE] Disconnected from server");
    digitalWrite(ONBOARD_LED, LOW);
  }
};

// ===============================
// CONNECT FUNCTION
// ===============================
bool connectToServer() {
  Serial.print("[BLE] Connecting to: ");
  Serial.println(myDevice->getName().c_str());
  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  pClient->connect(myDevice);

  BLERemoteService* service = pClient->getService(serviceUUID);
  if (service == nullptr) {
    Serial.println("[BLE] Failed to find main service");
    pClient->disconnect(); 
    return false;
  }

  Serial.println("[BLE] Service found. Registering characteristics...");
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

  connected = true;
  return true;
}

// ===============================
// ADVERTISEMENT CALLBACK
// ===============================
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Look for the "Wearables" server we created
    if (advertisedDevice.getName() == "Wearables") {
      Serial.print("[BLE] Found target device: ");
      Serial.println(advertisedDevice.toString().c_str());
      
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;
    }
  }
};

// ===============================
// SETUP 
// ===============================
void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, 16, 17); // RX=16, TX=17
  
  // Give Serial time to wake up
  unsigned long start = millis();
  while (!Serial && (millis() - start < 3000)) { delay(10); }

  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);
  
  Serial.println("\n========================================");
  Serial.println("BLE Master Client Node");
  Serial.println("========================================");

  BLEDevice::init("Wearables_Client");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  scan->setInterval(1349);
  scan->setWindow(749);
  scan->setActiveScan(true);
  
  Serial.println("Starting BLE scan...");
  scan->start(5, false);
}

// ===============================
// LOOP
// ===============================
void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("[MAIN] Successfully connected to Wearables server!");
    } else {
      Serial.println("[MAIN] Failed to connect to server");
      doScan = true;
    }
    doConnect = false;
  }

  if (!connected && doScan) {
    Serial.println("[MAIN] Connection lost/waiting, starting 5-second scan...");
    BLEDevice::getScan()->start(5, false); 
  }

  // --- 5 SECOND STATS PRINT ---
  if (millis() - lastStats >= 5000) {
      Serial.println();
      Serial.println("===== BLE CLIENT STATUS =====");
      Serial.print("Connected: ");
      Serial.println(connected ? "YES" : "NO");
      
      Serial.print("EEG packets:  "); Serial.print(eegPackets); 
      Serial.print("\tRate: "); Serial.print(eegSamples / 5); Serial.println(" samples/sec");
      
      Serial.print("IMU packets:  "); Serial.print(imuPackets);
      Serial.print("\tRate: "); Serial.print(imuPackets / 5); Serial.println(" pkts/sec");
      
      Serial.print("ECG packets:  "); Serial.print(ecgPackets);
      Serial.print("\tRate: "); Serial.print(ecgPackets / 5); Serial.println(" pkts/sec");
      
      Serial.print("Load packets: "); Serial.print(loadPackets);
      Serial.print("\tRate: "); Serial.print(loadPackets / 5); Serial.println(" pkts/sec");
      
      Serial.print("SPO2 packets: "); Serial.print(spo2Packets);
      Serial.print("\tRate: "); Serial.print(spo2Packets / 5); Serial.println(" pkts/sec");
      Serial.println("=============================");

      // Reset all counters
      eegPackets = 0;
      eegSamples = 0;
      imuPackets = 0;
      ecgPackets = 0;
      loadPackets = 0;
      spo2Packets = 0;
      
      lastStats = millis();
  }

  delay(10);
}