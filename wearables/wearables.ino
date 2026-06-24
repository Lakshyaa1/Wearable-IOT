#include <Arduino.h>
#include <bluefruit.h>

#define EEG_PIN A3

// =====================
// CONFIG
// =====================

constexpr uint32_t SAMPLE_INTERVAL_US = 10000; // 100 Hz
constexpr uint8_t BATCH_SIZE = 10;

// =====================
// PACKET FORMAT
// =====================

#pragma pack(push,1)
struct EEGPacket
{
    uint16_t samples[BATCH_SIZE];
};
#pragma pack(pop)

// =====================
// BLE
// =====================

BLEService eegService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

BLECharacteristic eegChar(
    0x2A99,
    CHR_PROPS_NOTIFY,
    sizeof(EEGPacket),
    sizeof(EEGPacket)
);

// =====================
// GLOBALS
// =====================

EEGPacket eegPacket;

uint8_t batchIndex = 0;

uint32_t lastSampleUs = 0;

uint32_t sampleCounter = 0;
uint32_t packetCounter = 0;

uint32_t lastReportMs = 0;

// =====================
// BLE CALLBACKS
// =====================

void connect_callback(uint16_t conn_handle)
{
    Serial.println("[BLE] Connected");
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    Serial.println("[BLE] Disconnected");
}

// =====================
// BLE SETUP
// =====================

void setupBLE()
{
    Bluefruit.begin();

    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

    Bluefruit.Periph.setConnectCallback(connect_callback);
    Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

    Bluefruit.setName("EEG_100HZ");

    eegService.begin();

    eegChar.setProperties(CHR_PROPS_NOTIFY);
    eegChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    eegChar.setFixedLen(sizeof(EEGPacket));
    eegChar.begin();

    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.addService(eegService);

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.start(0);

    Serial.println("[BLE] Advertising...");
}

// =====================
// SETUP
// =====================

void setup()
{
    
    Serial.begin(115200);
    analogReadResolution(12);
    unsigned long start = millis();

    while (!Serial && millis() - start < 5000)
    {
        delay(10);
    }

    pinMode(EEG_PIN, INPUT);

    Serial.println("EEG TX Starting");

    setupBLE();

    lastSampleUs = micros();
}

// =====================
// LOOP
// =====================

void loop()
{
    uint32_t nowUs = micros();

    if ((nowUs - lastSampleUs) < SAMPLE_INTERVAL_US)
        return;

    lastSampleUs += SAMPLE_INTERVAL_US;

    eegPacket.samples[batchIndex] = analogRead(EEG_PIN);
    Serial.println(eegPacket.samples[batchIndex]);
    batchIndex++;
    sampleCounter++;

    if (batchIndex >= BATCH_SIZE)
    {
        if (Bluefruit.connected())
        {
            eegChar.notify(
                (uint8_t*)&eegPacket,
                sizeof(EEGPacket)
            );

            packetCounter++;
        }

        batchIndex = 0;
    }

    if (millis() - lastReportMs >= 1000)
    {
        Serial.print("Samples/sec: ");
        Serial.print(sampleCounter);

        Serial.print(" | Packets/sec: ");
        Serial.print(packetCounter);

        Serial.print(" | Connected: ");
        Serial.println(Bluefruit.connected() ? "YES" : "NO");

        sampleCounter = 0;
        packetCounter = 0;
        lastReportMs = millis();
    }
}