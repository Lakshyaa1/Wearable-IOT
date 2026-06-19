#include "BLEDevice.h"

// ===============================
// UUID DEFINITIONS
// ===============================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID_imu((uint16_t)0x27A8);
static BLEUUID charUUID_ecg((uint16_t)0x2A37);
static BLEUUID charUUID_load((uint16_t)0x2A98);
static BLEUUID charUUID_spo2((uint16_t)0x2A8D);
static BLEUUID charUUID_eeg((uint16_t)0x2A99);  // ADD THIS

static BLEAdvertisedDevice* myDevice;
static BLERemoteCharacteristic* imuChar;
static BLERemoteCharacteristic* ecgChar;
static BLERemoteCharacteristic* loadChar;
static BLERemoteCharacteristic* spo2Char;
static BLERemoteCharacteristic* eegChar;  // ADD THIS

bool doConnect = false;
bool connected = false;
bool doScan = true;

// ===============================
// PARSER FUNCTION
// ===============================
void parseMessage(uint16_t type_, uint8_t* pData, size_t length) {
  char message[300];
  int idx = 0;
  message[0] = '\0'; // Clear buffer

  // Convert payload bytes to string
  String dataStr = "";
  for (size_t i = 0; i < length; i++) dataStr += (char)pData[i];

  // JSON start
  idx += sprintf(message + idx, "{");

  if (type_ == 1) { 
    // IMU data: "AX:%.2f AY:%.2f AZ:%.2f GX:%.1f GY:%.1f GZ:%.1f"
    float ax, ay, az, gx, gy, gz;
    int count = sscanf(dataStr.c_str(), "AX:%f AY:%f AZ:%f GX:%f GY:%f GZ:%f", &ax, &ay, &az, &gx, &gy, &gz);
    if (count == 6) {
      idx += sprintf(message + idx,
                     "\"dev\":\"IMU\",\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                     "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f",
                     ax, ay, az, gx, gy, gz);
     } 
    else {
      idx += sprintf(message + idx, "\"dev\":\"IMU\",\"raw\":\"%s\"", dataStr.c_str());
    }
  } 
  else if (type_ == 2) { 
    // ECG data
    int ecgVal = atoi(dataStr.c_str());
    idx += sprintf(message + idx, "\"dev\":\"ECG\",\"ecg\":%d", ecgVal);
  } 
  else if (type_ == 3) { 
    // Loadcell data
    float load = atof(dataStr.c_str());
    idx += sprintf(message + idx, "\"dev\":\"Load\",\"load\":%.2f", load);
  }
  else if (type_ == 4) {
    // SPO2 data: "Red,IR"
    long red, ir;
    int count = sscanf(dataStr.c_str(), "%ld,%ld", &red, &ir);
    if (count == 2) {
      idx += sprintf(message + idx, "\"dev\":\"SPO2\",\"red\":%ld,\"ir\":%ld", red, ir);
    } else {
      idx += sprintf(message + idx, "\"dev\":\"SPO2\",\"raw\":\"%s\"", dataStr.c_str());
    }
  }
  else if (type_ == 5) {
    // EEG data
    float eegVal = atof(dataStr.c_str());
    idx += sprintf(message + idx, "\"dev\":\"EEG\",\"eeg\":%.2f", eegVal);
  }
  
  // JSON end
  idx += sprintf(message + idx, "}\n");

  // Print to both Serial (USB) and Serial1 (UART)
  Serial.print(message);
  Serial1.print(message);
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
  else if (uuid.equals(charUUID_eeg)) type_ = 5;  // ADD THIS

  if (type_ > 0) parseMessage(type_, data, length);
  else Serial.println("Unknown characteristic notification received!");
}

// ===============================
// CLIENT CALLBACKS
// ===============================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) {
    Serial.println("[BLE] Connected to server");
    Serial1.println("[BLE] Connected to server");
  }
  void onDisconnect(BLEClient* pClient) {
    connected = false;
    Serial.println("[BLE] Disconnected from server");
    Serial1.println("[BLE] Disconnected from server");
  }
};

// ===============================
// CONNECT FUNCTION
// ===============================
bool connectToServer() {
  Serial.print("[BLE] Attempting to connect to: ");
  Serial.println(myDevice->getName().c_str());
  Serial1.print("[BLE] Connecting to: ");
  Serial1.println(myDevice->getName().c_str());

  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  pClient->connect(myDevice);

  BLERemoteService* service = pClient->getService(serviceUUID);
  if (service == nullptr) {
    Serial.println("[BLE] ERROR: Failed to find service UUID");
    Serial1.println("[BLE] ERROR: Failed to find service UUID");
    pClient->disconnect(); 
    return false;
  }
  Serial.println("[BLE] Service found!");

  // Get all characteristics
  imuChar = service->getCharacteristic(charUUID_imu);
  ecgChar = service->getCharacteristic(charUUID_ecg);
  loadChar = service->getCharacteristic(charUUID_load);
  spo2Char = service->getCharacteristic(charUUID_spo2);
  eegChar = service->getCharacteristic(charUUID_eeg);  // ADD THIS

  // Register for notifications
  if (imuChar && imuChar->canNotify()) {
    imuChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] IMU notifications enabled");
  }
  if (ecgChar && ecgChar->canNotify()) {
    ecgChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] ECG notifications enabled");
  }
  if (loadChar && loadChar->canNotify()) {
    loadChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] Load notifications enabled");
  }
  if (spo2Char && spo2Char->canNotify()) {
    spo2Char->registerForNotify(notifyCallback);
    Serial.println("[BLE] SPO2 notifications enabled");
  }
  if (eegChar && eegChar->canNotify()) {  // ADD THIS
    eegChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] EEG notifications enabled");
  }

  connected = true;
  return true;
}

// ===============================
// ADVERTISEMENT CALLBACK
// ===============================
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getName() == "Wearables") {
      Serial.print("[BLE] Found target device: ");
      Serial.println(advertisedDevice.toString().c_str());
      Serial1.println("[BLE] Found target device: Wearables");
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;
    }
  }
};

// ===============================
// SETUP + LOOP
// ===============================
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial1.begin(115200, SERIAL_8N1, 16, 17); // RX=16, TX=17
  delay(100);
  
  Serial.println("\n\n========================================");
  Serial.println("BLE Wearables Sensor Client");
  Serial.println("========================================");
  Serial.println("Initializing BLE...");
  
  Serial1.println("========================================");
  Serial1.println("BLE Wearables Sensor Client - UART Output");
  Serial1.println("========================================");

  BLEDevice::init("Wearables_Client");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  scan->setInterval(1349);
  scan->setWindow(749);
  scan->setActiveScan(true);
  
  Serial.println("Starting BLE scan...");
  Serial1.println("Starting BLE scan...");
  scan->start(5, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("[MAIN] Successfully connected to server!");
      Serial1.println("[MAIN] Successfully connected to server!");
    } else {
      Serial.println("[MAIN] Failed to connect to server");
      Serial1.println("[MAIN] Failed to connect to server");
    }
    doConnect = false;
  }

  if (!connected && doScan) {
    Serial.println("[MAIN] Connection lost, restarting scan...");
    Serial1.println("[MAIN] Connection lost, restarting scan...");
    BLEDevice::getScan()->start(0);
  }

  delay(10);
}