#include "BLEDevice.h"

#define ONBOARD_LED 2

#pragma pack(push,1)
struct EEGPacket {
    uint16_t samples[10];
};
#pragma pack(pop)

// ===============================
// UUID DEFINITIONS
// ===============================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID_eeg((uint16_t)0x2A99);  

static BLEAdvertisedDevice* myDevice;
static BLERemoteCharacteristic* eegChar;  

bool doConnect = false;
bool connected = false;
bool doScan = true;

// ===============================
// GLOBAL COUNTERS & BUFFERS
// ===============================
uint32_t eegPackets = 0;
uint32_t eegSamples = 0;
uint32_t lastStats = 0;

// ===============================
// PARSER FUNCTION
// ===============================
void parseMessage(uint8_t* pData, size_t length)
{
    if(length != sizeof(EEGPacket))
    {
        Serial.print("Bad EEG packet length: ");
        Serial.println(length);
        return;
    }

    EEGPacket packet;

    memcpy(
        &packet,
        pData,
        sizeof(packet)
    );

    eegPackets++;
    eegSamples += 10;

    char message[256];
    int idx = 0;

    idx += sprintf(
        message + idx,
        "{\"dev\":\"EEG\",\"samples\":["
    );

    for(int i = 0; i < 10; i++)
    {
        idx += sprintf(
            message + idx,
            "%u",
            packet.samples[i]
        );

        if(i < 9)
        {
            idx += sprintf(message + idx, ",");
        }
    }

    idx += sprintf(message + idx, "]}");

    //Serial.println(message);
    Serial1.println(message);
    Serial1.flush();
}

// ===============================
// NOTIFICATION CALLBACK
// ===============================
void notifyCallback(
    BLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t length,
    bool isNotify)
{
    if(characteristic->getUUID().equals(charUUID_eeg))
    {
        parseMessage(data, length);
    }
}

// ===============================
// CLIENT CALLBACKS
// ===============================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) {
    Serial.println("[BLE] Connected to server");
    Serial1.println("[BLE] Connected to server");
    digitalWrite(ONBOARD_LED, HIGH); // Turn LED ON upon BLE connection
  }
  void onDisconnect(BLEClient* pClient) {
    connected = false;
    doScan = true; // <--- ADDED: Tells the main loop to start scanning again
    Serial.println("[BLE] Disconnected from server");
    Serial1.println("[BLE] Disconnected from server");
    digitalWrite(ONBOARD_LED, LOW); // Turn LED OFF upon disconnect
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

  // Get only the EEG characteristic
  eegChar = service->getCharacteristic(charUUID_eeg);

  // Register for notifications
  if (eegChar && eegChar->canNotify()) {
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
    if (advertisedDevice.getName() == "EEG_100HZ") {
      Serial.print("[BLE] Found target device: ");
      Serial.println(advertisedDevice.toString().c_str());
      Serial1.println("[BLE] Found target device: EEG_100HZ");
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false; // Stop scanning once we find it
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

  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);
  
  Serial.println("\n\n========================================");
  Serial.println("BLE EEG Sensor Client");
  Serial.println("========================================");
  Serial.println("Initializing BLE...");
  
  Serial1.println("========================================");
  Serial1.println("BLE EEG Sensor Client - UART Output");
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
      doScan = true; // <--- ADDED: If connection fails, force a re-scan
    }
    doConnect = false;
  }

  // <--- UPDATED: Scans in 5-second bursts to allow the loop to keep breathing
  if (!connected && doScan) {
    Serial.println("[MAIN] Connection lost/failed, starting 5-second scan...");
    Serial1.println("[MAIN] Connection lost/failed, starting scan...");
    BLEDevice::getScan()->start(5, false); 
  }

  // ===============================
  // 5-SECOND STATUS PRINT
  // ===============================
  if (millis() - lastStats >= 5000) {
      Serial.println();
      Serial.println("===== BLE CLIENT STATUS =====");

      Serial.print("Connected: ");
      Serial.println(connected ? "YES" : "NO");

      Serial.print("EEG packets: ");
      Serial.println(eegPackets);

      Serial.print("EEG samples: ");
      Serial.println(eegSamples);

      Serial.print("EEG rate: ");
      Serial.print(eegSamples / 5);
      Serial.println(" samples/sec");

      Serial.println("=============================");

      // Reset counters
      eegPackets = 0;
      eegSamples = 0;

      lastStats = millis();
  }

  delay(10);
}