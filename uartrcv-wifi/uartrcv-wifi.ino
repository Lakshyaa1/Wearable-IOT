#include "WiFi.h"
#include <WebSocketsClient.h>



#include "WiFi.h"
#include <WebSocketsClient.h>

#define BATCH_SIZE 1
#define MAX_BUFFER_SIZE 512
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

// Statistics
volatile unsigned long dataReceived = 0;
volatile unsigned long dataSent = 0;
volatile unsigned long dataDropped = 0;
volatile unsigned long sendFailed = 0;

// Rate and Queue Tracking
volatile uint32_t uartMsgs = 0;
volatile uint32_t wsMsgs = 0;
volatile uint32_t maxQueueDepth = 0;

void readUARTTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  int index = 0;

  Serial.println("[UART Task] Started");

  for (;;) {
    // Track high-water mark for queue depth
    uint32_t depth = uxQueueMessagesWaiting(uartQueue);
    if (depth > maxQueueDepth) {
      maxQueueDepth = depth;
    }

    while (Serial1.available()) {
      char rxChar = Serial1.read();
      
      // Start of JSON message
      if (rxChar == '{') {
        index = 0;
        uartData[index++] = rxChar;
      } 
      // End of JSON message
      else if (rxChar == '\n' && index > 0) {
        uartData[index++] = rxChar;
        uartData[index] = '\0';
        
        dataReceived++;
        uartMsgs++; // Track for rate calculation

        if (uartQueue != NULL) {
          // Non-blocking send - if queue full, drop oldest data
          if (xQueueSend(uartQueue, uartData, 0) != pdTRUE) {
            dataDropped++;
            
            // Try to receive and discard oldest item, then queue new one
            char dummy[MAX_BUFFER_SIZE];
            if (xQueueReceive(uartQueue, dummy, 0) == pdTRUE) {
              xQueueSend(uartQueue, uartData, 0);
            }
          }
        }
        
        index = 0;
      } 
      // Accumulate message bytes
      else if (index > 0) {
        if (index < MAX_BUFFER_SIZE - 1) {
          uartData[index++] = rxChar;
        } else {
          Serial.printf("[UART] ✗ Buffer overflow! Dropping message\n");
          dataDropped++;
          index = 0;
        }
      }
    }

    // Reduced polling delay from 5ms to 1ms
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void sendWebSocketTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  int consecutiveFailures = 0;
  
  Serial.println("[WS Task] Started");
  
  for (;;) {
    // Wait for data in queue with timeout
    if (xQueueReceive(uartQueue, uartData, pdMS_TO_TICKS(500))) {
      
      // Wait for connection with exponential backoff
      int waitCount = 0;
      int maxWait = 100;  // 10 seconds max
      
      while (!isConnected && waitCount < maxWait) {
        checkWifi();
        vTaskDelay(pdMS_TO_TICKS(100));
        waitCount++;
      }
      
      if (isConnected) {
        if (strlen(uartData) > 0) {
          bool sent = webSocket.sendTXT(uartData);
          
          if (sent) {
            dataSent++;
            wsMsgs++; // Track for rate calculation
            consecutiveFailures = 0;
            digitalWrite(ONBOARD_LED, HIGH);
          } else {
            sendFailed++;
            consecutiveFailures++;
            digitalWrite(ONBOARD_LED, LOW);
            
            if (consecutiveFailures > 5) {
              consecutiveFailures = 0;
            }
          }
        }
      } else {
        sendFailed++;
        digitalWrite(ONBOARD_LED, LOW);
      }
    } 
    else {
      // No data available, just keep WebSocket alive
      checkWifi();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void statusTask(void* parameter) {
  Serial.println("[Status Task] Started - reporting every 5s");
  
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));  // Changed to 5 seconds to match BLE ESP
    
    // Calculate Rates
    uint32_t uartRate = uartMsgs / 5;
    uint32_t wsRate = wsMsgs / 5;
    uint32_t wsSamples = wsRate * 10; // Assuming 10 samples per packet based on the BLE pipeline

    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║        SYSTEM STATUS REPORT (5s)       ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ WiFi Connected: %s\n", WiFi.status() == WL_CONNECTED ? "✓ YES        " : "✗ NO         ");
    Serial.printf("║ WebSocket Connected: %s\n", isConnected ? "✓ YES        " : "✗ NO         ");
    Serial.printf("║ IP Address: %-28s ║\n", WiFi.localIP().toString().c_str());
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ UART Rx Rate: %-4lu pkts/sec             ║\n", uartRate);
    Serial.printf("║ WS Tx Rate:   %-4lu pkts/sec             ║\n", wsRate);
    Serial.printf("║ WS Tx Rate:   %-4lu samples/sec          ║\n", wsSamples);
    Serial.printf("║ Current Queue Depth: %3u / 50          ║\n", uxQueueMessagesWaiting(uartQueue));
    Serial.printf("║ Queue Max Depth:     %-18lu║\n", maxQueueDepth);
    Serial.printf("║ Data Dropped:        %-18lu║\n", dataDropped);
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ Total Received (UART): %-15lu ║\n", dataReceived);
    Serial.printf("║ Total Sent (WS): %-21lu ║\n", dataSent);
    Serial.printf("║ Send Failures: %-23lu ║\n", sendFailed);
    Serial.println("╚════════════════════════════════════════╝\n");

    // Reset windowed trackers
    uartMsgs = 0;
    wsMsgs = 0;
    maxQueueDepth = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 WebSocket Client - Wearables   ║");
  Serial.println("║      Sensor Data Aggregator v2.1       ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  Serial1.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);

  // Set queue size explicitly to 50
  uartQueue = xQueueCreate(50, MAX_BUFFER_SIZE * sizeof(char));
  if (uartQueue == NULL) {
    Serial.println("[Setup] ✗ Queue creation failed!");
    while(1) {
      digitalWrite(ONBOARD_LED, HIGH);
      delay(100);
      digitalWrite(ONBOARD_LED, LOW);
      delay(100);
    }
  }
  Serial.println("[Setup] ✓ Queue created (capacity: 50 messages)");

  // Connect to WiFi
  Serial.println("[Setup] Connecting to WiFi...");
  init_wifi();
  Serial.print("[Setup] ✓ WiFi connected! IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("[Setup] RSSI: %d dBm\n", WiFi.RSSI());

  // Initialize WebSocket
  Serial.printf("[Setup] Connecting to ws://%s:%d%s\n", ws_host, ws_port, ws_path);
  webSocket.begin(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  // Enable heartbeat to keep connection alive
  webSocket.enableHeartbeat(15000, 3000, 2);

  // Create tasks
  xTaskCreatePinnedToCore(readUARTTask, "Read UART", 4096, NULL, 2, &readUARTTask_h, 0);
  xTaskCreatePinnedToCore(sendWebSocketTask, "Send WebSocket", 8192, NULL, 2, &sendWebSocketTask_h, 1);
  xTaskCreatePinnedToCore(statusTask, "Status Monitor", 4096, NULL, 1, &statusTask_h, 1);

  Serial.println("[Setup] ✓ All tasks created and running!");
  Serial.println("[Setup] ✓ Setup complete!\n");
  digitalWrite(ONBOARD_LED, HIGH);
}

void loop() {
  webSocket.loop();
  vTaskDelay(pdMS_TO_TICKS(1));
}

void init_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    attempts++;
    
    if (attempts > 60) {
      Serial.println("\n[WiFi] ✗ Failed to connect - restarting");
      delay(1000);
      ESP.restart();
    }
  }
  Serial.println();
}

void checkWifi() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  
  if (now - lastCheck < 5000) {
    return;
  }
  lastCheck = now;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] ⚠ Connection lost!");
    digitalWrite(ONBOARD_LED, LOW);
    isConnected = false;
    
    WiFi.reconnect();
    int attempts = 0;
    
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      attempts++;
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] ✓ Reconnected");
      Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
      webSocket.begin(ws_host, ws_port, ws_path);
    } else {
      Serial.println("[WiFi] ✗ Reconnect failed");
    }
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS Event] ✗ WebSocket Disconnected");
      isConnected = false;
      digitalWrite(ONBOARD_LED, LOW);
      break;
      
    case WStype_CONNECTED:
      Serial.printf("[WS Event] ✓ WebSocket Connected to: %s\n", payload);
      isConnected = true;
      digitalWrite(ONBOARD_LED, HIGH);
      break;
      
    case WStype_TEXT:
      break;
      
    case WStype_BIN:
      break;
      
    case WStype_ERROR:
      Serial.printf("[WS Event] ✗ WebSocket Error\n");
      isConnected = false;
      digitalWrite(ONBOARD_LED, LOW);
      break;
      
    case WStype_PING:
      break;
      
    case WStype_PONG:
      break;
      
    default:
      Serial.printf("[WS Event] Unknown event type: %d\n", type);
      break;
  }
}