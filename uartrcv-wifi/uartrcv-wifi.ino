#include "WiFi.h"
#include <WebSocketsClient.h>

#define MAX_BUFFER_SIZE 512
#define BATCH_BUFFER_SIZE 8192
#define ONBOARD_LED 2

const char* ssid = "NCAIR IOT";
const char* password = "Asim@123Tewari";
const char* ws_host = "192.168.0.180"; 
const uint16_t ws_port = 1234;
const char* ws_path = "/";

WebSocketsClient webSocket;
bool isConnected = false;

QueueHandle_t uartQueue = NULL;
TaskHandle_t readUARTTask_h = NULL;
TaskHandle_t sendWebSocketTask_h = NULL;
TaskHandle_t statusTask_h = NULL;

volatile unsigned long dataReceived = 0;
volatile unsigned long dataSent = 0;
volatile unsigned long dataDropped = 0;
volatile unsigned long sendFailed = 0;
volatile uint32_t uartEEGMsgs = 0;
volatile uint32_t wsEEGMsgs = 0;
volatile uint32_t uartMsgs = 0;
volatile uint32_t uartBytesWindow = 0; 
volatile uint32_t wsMsgs = 0;
volatile uint32_t wsBytesWindow = 0;   
volatile uint32_t maxQueueDepth = 0;

// Forward declaration
void checkWifi();

void readUARTTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  int index = 0;

  for (;;) {
    uint32_t depth = uxQueueMessagesWaiting(uartQueue);
    if (depth > maxQueueDepth) maxQueueDepth = depth;

    while (Serial1.available()) {
      char rxChar = Serial1.read();
      uartBytesWindow++; 
      
      if (rxChar == '{') { index = 0; uartData[index++] = rxChar; } 
      else if (rxChar == '\n' && index > 0) {
        uartData[index++] = rxChar;
        uartData[index] = '\0';
        
        dataReceived++;
        uartMsgs++;
        if (strstr(uartData, "\"dev\":\"EEG\"")) uartEEGMsgs++;

        if (uartQueue != NULL) {
          if (xQueueSend(uartQueue, uartData, 0) != pdTRUE) {
            dataDropped++;
            char dummy[MAX_BUFFER_SIZE];
            if (xQueueReceive(uartQueue, dummy, 0) == pdTRUE) xQueueSend(uartQueue, uartData, 0);
          }
        }
        index = 0;
      } 
      else if (index > 0 && index < MAX_BUFFER_SIZE - 1) {
        uartData[index++] = rxChar;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void sendWebSocketTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  static char batchBuffer[BATCH_BUFFER_SIZE]; 
  int batchCount = 0;
  int batchEEGCount = 0;
  int pos = 0;
  
  batchBuffer[pos++] = '['; 
  
  for (;;) {
    if (xQueueReceive(uartQueue, uartData, pdMS_TO_TICKS(1))) {
      bool isEEG = (strstr(uartData, "\"dev\":\"EEG\"") != nullptr);
      if (isEEG) batchEEGCount++;

      int len = strlen(uartData);
      if (pos + len + 2 < BATCH_BUFFER_SIZE) {
        if (pos > 1) batchBuffer[pos++] = ',';
        memcpy(batchBuffer + pos, uartData, len);
        pos += len;
        batchCount++;
      }
      
      if (batchCount >= 20 || uxQueueMessagesWaiting(uartQueue) == 0) {
        batchBuffer[pos++] = ']';
        batchBuffer[pos] = '\0';
        
        if (isConnected) {
          uint32_t t0 = micros();
          if (webSocket.sendTXT(batchBuffer)) {
            uint32_t dt = micros() - t0;
            if (dt > 5000) Serial.printf("WS send slow: %lu us\n", dt);
            
            dataSent += batchCount;
            wsMsgs += batchCount;
            wsEEGMsgs += batchEEGCount;
            wsBytesWindow += pos; 
            digitalWrite(ONBOARD_LED, HIGH);
          } else {
            sendFailed++;
          }
        }
        pos = 0;
        batchBuffer[pos++] = '[';
        batchCount = 0;
        batchEEGCount = 0;
      }
    } else {
      checkWifi();
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void statusTask(void* parameter) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.printf("║ UART Rx Rate: %-4lu pkts/sec           ║\n", uartMsgs / 5);
    Serial.printf("║ WS Tx Rate:   %-4lu pkts/sec           ║\n", wsMsgs / 5);
    Serial.printf("║ UART EEG Rx:  %-4lu pkts/sec           ║\n", uartEEGMsgs / 5);
    Serial.printf("║ WS EEG Tx:    %-4lu pkts/sec           ║\n", wsEEGMsgs / 5);
    Serial.printf("║ Current Queue: %3u / 100              ║\n", uxQueueMessagesWaiting(uartQueue));
    Serial.printf("║ Data Dropped:  %-18lu          ║\n", dataDropped);
    Serial.println("╚════════════════════════════════════════╝\n");
    uartMsgs = 0; wsMsgs = 0; uartEEGMsgs = 0; wsEEGMsgs = 0;
    uartBytesWindow = 0; wsBytesWindow = 0; maxQueueDepth = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, 16, 17);
  uartQueue = xQueueCreate(100, MAX_BUFFER_SIZE * sizeof(char));
  init_wifi();
  webSocket.begin(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  xTaskCreatePinnedToCore(readUARTTask, "Read", 4096, NULL, 2, &readUARTTask_h, 0);
  xTaskCreatePinnedToCore(sendWebSocketTask, "Send", 8192, NULL, 2, &sendWebSocketTask_h, 1);
  xTaskCreatePinnedToCore(statusTask, "Status", 4096, NULL, 1, &statusTask_h, 1);
}

void loop() { webSocket.loop(); }
void init_wifi() { WiFi.mode(WIFI_STA); WiFi.begin(ssid, password); while (WiFi.status() != WL_CONNECTED) delay(500); }
void checkWifi() { if (WiFi.status() != WL_CONNECTED) WiFi.reconnect(); }
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) { if(type == WStype_CONNECTED) isConnected = true; else if(type == WStype_DISCONNECTED) isConnected = false; }