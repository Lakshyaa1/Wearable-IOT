#include "WiFi.h"
#include <WebSocketsClient.h>

#define BATCH_SIZE 1
#define MAX_BUFFER_SIZE 256
#define ONBOARD_LED 2

const char* ssid = "MTNL";
const char* password = "20851137";

const char* ws_host = "192.168.1.16"; // Note: Fixed the typo "1192" from original code here
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

void readUARTTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  int index = 0;

  Serial.println("[UART Task] Started");

  for (;;) {
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

        if (uartQueue != NULL) {
          // Non-blocking send - if queue full, drop oldest data
          if (xQueueSend(uartQueue, uartData, 0) != pdTRUE) {
            Serial.printf("[UART] ⚠ Queue full (size: %d) - dropping data\n", 
                         uxQueueMessagesWaiting(uartQueue));
            dataDropped++;
            
            // Try to receive and discard oldest item, then queue new one
            char dummy[MAX_BUFFER_SIZE];
            // FIX: Removed the & before dummy
            if (xQueueReceive(uartQueue, dummy, 0) == pdTRUE) {
              xQueueSend(uartQueue, uartData, 0);
              Serial.println("[UART] Discarded oldest, queued new");
            }
          } else {
            Serial.printf("[UART] ✓ Queued: %.50s...\n", uartData);
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

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void sendWebSocketTask(void* parameter) {
  char uartData[MAX_BUFFER_SIZE];
  int consecutiveFailures = 0;
  
  Serial.println("[WS Task] Started");
  
  for (;;) {
    // Wait for data in queue with timeout
    // FIX: Removed the & before uartData
    if (xQueueReceive(uartQueue, uartData, pdMS_TO_TICKS(500))) {
      Serial.printf("[WS Task] Got data: %.60s\n", uartData);
      
      // Wait for connection with exponential backoff
      int waitCount = 0;
      int maxWait = 100;  // 10 seconds max
      
      while (!isConnected && waitCount < maxWait) {
        checkWifi();
        // FIX: Removed webSocket.loop() from here
        vTaskDelay(pdMS_TO_TICKS(100));
        waitCount++;
        
        if (waitCount % 20 == 0) {
          Serial.printf("[WS Task] ⏳ Waiting for connection... (%d/100)\n", waitCount);
        }
      }
      
      if (isConnected) {
        Serial.println("[WS Task] 📤 Sending...");
        
        // FIX: Added length check before sending
        if (strlen(uartData) > 0) {
          bool sent = webSocket.sendTXT(uartData);
          
          if (sent) {
            Serial.println("[WS Task] ✓ Sent successfully");
            dataSent++;
            consecutiveFailures = 0;
            digitalWrite(ONBOARD_LED, HIGH);
          } else {
            Serial.println("[WS Task] ✗ Send failed");
            sendFailed++;
            consecutiveFailures++;
            digitalWrite(ONBOARD_LED, LOW);
            
            // FIX: Removed dangerous manual disconnect()/begin() logic
            if (consecutiveFailures > 5) {
              Serial.println("[WS Task] Too many failures");
              consecutiveFailures = 0;
            }
          }
        }
      } else {
        Serial.println("[WS Task] ✗ Timeout waiting for connection (10s)");
        sendFailed++;
        digitalWrite(ONBOARD_LED, LOW);
      }
    } 
    else {
      // No data available, just keep WebSocket alive (handled by main loop now)
      checkWifi();
      // FIX: Removed webSocket.loop() from here
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void statusTask(void* parameter) {
  Serial.println("[Status Task] Started - reporting every 30s");
  
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║       SYSTEM STATUS REPORT (30s)       ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ WiFi Connected: %s\n", 
                 WiFi.status() == WL_CONNECTED ? "✓ YES        " : "✗ NO         ");
    Serial.printf("║ WebSocket Connected: %s\n", 
                 isConnected ? "✓ YES        " : "✗ NO         ");
    Serial.printf("║ IP Address: %-28s ║\n", WiFi.localIP().toString().c_str());
    Serial.printf("║ RSSI (WiFi): %d dBm                  ║\n", WiFi.RSSI());
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ Data Received (UART): %-17lu ║\n", dataReceived);
    Serial.printf("║ Data Sent (WS): %-23lu ║\n", dataSent);
    Serial.printf("║ Data Dropped: %-24lu ║\n", dataDropped);
    Serial.printf("║ Send Failures: %-23lu ║\n", sendFailed);
    Serial.printf("║ Queue Size: %u / 100                  ║\n", 
                 uxQueueMessagesWaiting(uartQueue));
    Serial.println("╚════════════════════════════════════════╝\n");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 WebSocket Client - Wearables   ║");
  Serial.println("║      Sensor Data Aggregator v2.0       ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  Serial1.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);

  // FIX: Increased queue size to 100 for debugging
  uartQueue = xQueueCreate(100, MAX_BUFFER_SIZE * sizeof(char));
  if (uartQueue == NULL) {
    Serial.println("[Setup] ✗ Queue creation failed!");
    while(1) {
      digitalWrite(ONBOARD_LED, HIGH);
      delay(100);
      digitalWrite(ONBOARD_LED, LOW);
      delay(100);
    }
  }
  Serial.println("[Setup] ✓ Queue created (capacity: 100 messages)");

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

  // Create tasks with optimized priorities
  xTaskCreatePinnedToCore(readUARTTask, "Read UART", 4096, NULL, 2, &readUARTTask_h, 0);
  xTaskCreatePinnedToCore(sendWebSocketTask, "Send WebSocket", 8192, NULL, 2, &sendWebSocketTask_h, 1);
  xTaskCreatePinnedToCore(statusTask, "Status Monitor", 4096, NULL, 1, &statusTask_h, 1);

  Serial.println("[Setup] ✓ All tasks created and running!");
  Serial.println("[Setup] ✓ Setup complete!\n");
  digitalWrite(ONBOARD_LED, HIGH);
}

void loop() {
  // FIX: This is now the ONLY place where webSocket.loop() is called
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
  
  // Only check every 5 seconds to avoid hammering
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
      Serial.printf("[WS Event] 📥 Received (%zu bytes): %s\n", length, payload);
      break;
      
    case WStype_BIN:
      Serial.printf("[WS Event] 📥 Received binary (%zu bytes)\n", length);
      break;
      
    case WStype_ERROR:
      Serial.printf("[WS Event] ✗ WebSocket Error\n");
      isConnected = false;
      digitalWrite(ONBOARD_LED, LOW);
      break;
      
    case WStype_PING:
      Serial.println("[WS Event] 📤 Ping sent");
      break;
      
    case WStype_PONG:
      Serial.println("[WS Event] 📥 Pong received");
      break;
      
    default:
      Serial.printf("[WS Event] Unknown event type: %d\n", type);
      break;
  }
}