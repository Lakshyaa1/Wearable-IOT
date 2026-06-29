/**
 * esp32-vn-wifi-provisioning.ino
 *
 * Single-ESP32 firmware for BioAmp EXG Pill:
 *  1. On first boot the ESP opens soft-AP "ESP32-Config" and serves a captive-
 *     portal config page where you enter WiFi SSID, password, and host laptop IP.
 *     Credentials are saved to NVS (Preferences) and survive reboots.
 *  2. After provisioning the ESP connects to the configured WiFi and continuously
 *     samples the BioAmp EXG Pill analog output on GPIO 39 (VN pin, ADC1 CH3)
 *     at 100 Hz and forwards readings in batches over WebSocket to
 *     ws://<host_ip>:1234/
 *
 * Hardware
 * --------
 *  BioAmp EXG Pill OUT  -->  ESP32 GPIO 39 (VN)   [ADC1 channel 3, input-only]
 *  Onboard LED          -->  GPIO 2
 *
 * Dependencies (install via Arduino Library Manager)
 * ---------------------------------------------------
 *  - arduinoWebSockets  (Markus Sattler)  >= 2.3.6
 *  DNSServer and WebServer are part of the ESP32 Arduino core.
 *
 * WiFi Provisioning
 * -----------------
 *  Connect to "ESP32-Config" (open) -> captive portal opens automatically.
 *  If not, browse to http://192.168.4.1
 *  Submit SSID, password, host IP -> ESP reboots into streaming mode.
 *
 *  To re-provision at any time: while the ESP is running normally, press and
 *  hold the BOOT button (GPIO 0) for 3 seconds. It will clear credentials and restart.
 *
 * Data format (each WebSocket message is a JSON array of samples)
 * ---------------------------------------------------------------
 *  [{"dev":"EXG","v":2048,"t":12345},{"dev":"EXG","v":2051,"t":12355}, ...]
 *   v  = 12-bit ADC value (0-4095, ~3.3 V full scale)
 *   t  = millis() timestamp on the ESP
 */

// ---------------------------------------------------------------------------
//  Includes
// ---------------------------------------------------------------------------
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
#define ONBOARD_LED       2
#define ADC_PIN           39      // GPIO39 / VN – BioAmp EXG Pill output
#define BOOT_BUTTON_PIN   0

#define SAMPLE_RATE_HZ    250     // target sample rate (Nyquist limit = 125 Hz)
#define SAMPLE_INTERVAL_MS (1000 / SAMPLE_RATE_HZ)   // 4 ms

#define MAX_SAMPLE_STR    48      // max chars per JSON sample string
#define QUEUE_DEPTH       500     // ring-buffer depth (2 seconds at 250 Hz)
#define BATCH_BUFFER_SIZE 8192
#define BATCH_THRESHOLD   50      // flush after 50 samples (~200 ms)

#define AP_SSID  "ESP32-Config"
#define AP_PASS  ""               // open network

#define WS_PORT  1234
#define WS_PATH  "/"

#define NVS_NS        "wificfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"
#define NVS_KEY_HOST  "host"

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
Preferences      prefs;
WebServer        httpServer(80);
DNSServer        dnsServer;
WebSocketsClient webSocket;

bool isConnected = false;

QueueHandle_t sampleQueue    = NULL;
TaskHandle_t  adcTask_h      = NULL;
TaskHandle_t  sendWSTask_h   = NULL;
TaskHandle_t  statusTask_h   = NULL;

// Stats counters (reset every STATUS_INTERVAL_S seconds)
#define STATUS_INTERVAL_S  5
volatile uint32_t samplesAcquired = 0;   // ADC readings taken
volatile uint32_t samplesSent     = 0;   // samples successfully sent via WS
volatile uint32_t samplesDropped  = 0;   // dropped (queue full)
volatile uint32_t wsSendFails     = 0;

// ---------------------------------------------------------------------------
//  Provisioning HTML
// ---------------------------------------------------------------------------
static const char* PROVISION_HTML =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 WiFi Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;"
"display:flex;align-items:center;justify-content:center;min-height:100vh;padding:1rem}"
".card{background:#1e293b;border:1px solid #334155;border-radius:16px;"
"padding:2.5rem 2rem;width:100%;max-width:420px;box-shadow:0 25px 50px rgba(0,0,0,.5)}"
"h1{font-size:1.5rem;font-weight:700;margin-bottom:.4rem;color:#38bdf8}"
"p.sub{font-size:.85rem;color:#94a3b8;margin-bottom:1.8rem}"
"label{display:block;font-size:.8rem;font-weight:600;color:#94a3b8;margin-bottom:.35rem;"
"letter-spacing:.05em;text-transform:uppercase}"
"input{width:100%;padding:.65rem .9rem;background:#0f172a;border:1px solid #475569;"
"border-radius:8px;color:#e2e8f0;font-size:.95rem;margin-bottom:1.1rem;outline:none;"
"transition:border-color .2s}"
"input:focus{border-color:#38bdf8}"
"button{width:100%;padding:.8rem;background:linear-gradient(135deg,#0ea5e9,#6366f1);"
"color:#fff;font-size:1rem;font-weight:700;border:none;border-radius:8px;cursor:pointer;"
"letter-spacing:.04em;transition:opacity .2s}"
"button:hover{opacity:.88}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h1>&#x1F4F6; ESP32 WiFi Setup</h1>"
"<p class='sub'>Enter your WiFi credentials and the host laptop IP where EXG data will be streamed.</p>"
"<form method='POST' action='/save'>"
"<label for='ssid'>WiFi SSID</label>"
"<input id='ssid' name='ssid' type='text' placeholder='MyNetwork' required autocomplete='off'>"
"<label for='pass'>WiFi Password</label>"
"<input id='pass' name='pass' type='password' placeholder='(leave blank if open)' autocomplete='off'>"
"<label for='host'>Host Laptop IP</label>"
"<input id='host' name='host' type='text' placeholder='192.168.x.x' required>"
"<button type='submit'>Save &amp; Connect &#x2192;</button>"
"</form>"
"</div>"
"</body>"
"</html>";

// ---------------------------------------------------------------------------
//  HTTP handlers
// ---------------------------------------------------------------------------
void handleRoot() {
  httpServer.send(200, "text/html", PROVISION_HTML);
}

void handleSave() {
  if (!httpServer.hasArg("ssid") || !httpServer.hasArg("host")) {
    httpServer.send(400, "text/plain", "Missing fields");
    return;
  }
  String ssid = httpServer.arg("ssid");
  String pass = httpServer.arg("pass");
  String host = httpServer.arg("host");
  ssid.trim(); pass.trim(); host.trim();

  if (ssid.length() == 0 || host.length() == 0) {
    httpServer.send(400, "text/plain", "SSID and Host IP are required");
    return;
  }

  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_KEY_SSID, ssid);
  prefs.putString(NVS_KEY_PASS, pass);
  prefs.putString(NVS_KEY_HOST, host);
  prefs.end();

  Serial.printf("[Provision] Saved -> SSID: %s | Host: %s\n", ssid.c_str(), host.c_str());

  String ok =
    "<html><body style='font-family:sans-serif;background:#0f172a;color:#4ade80;"
    "display:flex;align-items:center;justify-content:center;height:100vh;margin:0'>"
    "<div style='text-align:center'>"
    "<h2>&#x2705; Saved!</h2>"
    "<p style='color:#94a3b8;margin-top:.5rem'>Connecting to <b style='color:#38bdf8'>"
    + ssid + "</b>.<br>Reconnect your device to that network.</p>"
    "</div></body></html>";
  httpServer.send(200, "text/html", ok);
  delay(1500);
  ESP.restart();
}

void handleCaptivePortal() {
  httpServer.sendHeader("Location", "http://192.168.4.1/");
  httpServer.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
//  Provisioning mode  (does not return)
// ---------------------------------------------------------------------------
void runProvisioningMode() {
  Serial.println("[Provision] Starting soft-AP: " AP_SSID);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) > 0 ? AP_PASS : nullptr);
  delay(100);

  IPAddress apIP(192, 168, 4, 1);
  Serial.print("[Provision] AP IP: "); Serial.println(WiFi.softAPIP());

  dnsServer.start(53, "*", apIP);     // wildcard DNS -> captive portal

  httpServer.on("/",     HTTP_GET,  handleRoot);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.onNotFound(handleCaptivePortal);
  httpServer.begin();

  Serial.println("[Provision] Connect to 'ESP32-Config', portal opens automatically.");
  Serial.println("[Provision] Or browse to http://192.168.4.1");

  uint32_t lastBlink = 0;
  bool     ledState  = false;
  while (true) {
    dnsServer.processNextRequest();
    httpServer.handleClient();
    if (millis() - lastBlink >= 200) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(ONBOARD_LED, ledState);
    }
  }
}

// ---------------------------------------------------------------------------
//  WiFi helpers
// ---------------------------------------------------------------------------
bool connectWiFi(const String& ssid, const String& pass) {
  Serial.printf("[WiFi] Connecting to \"%s\"", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : nullptr);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("[WiFi] FAILED.");
  return false;
}

void checkWifi() {
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
}

// ---------------------------------------------------------------------------
//  WebSocket event
// ---------------------------------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      isConnected = true;
      Serial.println("[WS] Connected");
      break;
    case WStype_DISCONNECTED:
      isConnected = false;
      Serial.println("[WS] Disconnected");
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
//  ADC sampling task  (Core 0)
//  Reads BioAmp EXG Pill analog output on GPIO39 at SAMPLE_RATE_HZ (100 Hz).
//  Each sample is formatted as a compact JSON object and pushed onto the queue.
// ---------------------------------------------------------------------------
void adcSampleTask(void* parameter) {
  char sample[MAX_SAMPLE_STR];

  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    // Precise 10 ms interval (100 Hz) regardless of execution time
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));

    uint16_t adcVal = (uint16_t)analogRead(ADC_PIN);
    uint32_t ts     = (uint32_t)millis();

    // DEBUG: print every sample so you can see real-time signal variation.
    // Comment this line out once readings look correct.
    Serial.printf("[ADC] raw=%4u  %.3f V\n", adcVal, adcVal * 3.3f / 4095.0f);

    // Format: {"dev":"EXG","v":XXXX,"t":XXXXXXXXXX}
    int len = snprintf(sample, sizeof(sample),
                       "{\"dev\":\"EXG\",\"v\":%u,\"t\":%lu}",
                       adcVal, ts);

    if (len <= 0 || len >= (int)sizeof(sample)) continue; // shouldn't happen

    samplesAcquired++;

    if (xQueueSend(sampleQueue, sample, 0) != pdTRUE) {
      // Queue full: drop oldest, insert newest
      samplesDropped++;
      char dummy[MAX_SAMPLE_STR];
      if (xQueueReceive(sampleQueue, dummy, 0) == pdTRUE)
        xQueueSend(sampleQueue, sample, 0);
    }
  }
}

// ---------------------------------------------------------------------------
//  WebSocket send task  (Core 1)
//  IMPORTANT: webSocket.loop() is called here (NOT in Arduino loop()) so that
//  both loop() and sendTXT() run on the same core/task and the TCP handshake
//  is never starved by higher-priority FreeRTOS tasks.
// ---------------------------------------------------------------------------
void sendWSTask(void* parameter) {
  char sample[MAX_SAMPLE_STR];
  static char batchBuffer[BATCH_BUFFER_SIZE];
  int batchCount = 0;
  int pos        = 0;

  batchBuffer[pos++] = '[';

  for (;;) {
    // Drive the WebSocket state machine EVERY iteration - this is what
    // establishes the TCP connection and keeps it alive.
    webSocket.loop();

    if (xQueueReceive(sampleQueue, sample, pdMS_TO_TICKS(5))) {
      int len = strlen(sample);
      if (pos + len + 2 < BATCH_BUFFER_SIZE) {
        if (pos > 1) batchBuffer[pos++] = ',';
        memcpy(batchBuffer + pos, sample, len);
        pos += len;
        batchCount++;
      }

      bool flush = (batchCount >= BATCH_THRESHOLD) ||
                   (uxQueueMessagesWaiting(sampleQueue) == 0);

      if (flush && batchCount > 0) {
        batchBuffer[pos++] = ']';
        batchBuffer[pos]   = '\0';

        // DEBUG: show WS state and batch details before sending
        // Comment out once data is confirmed arriving on the server.
        Serial.printf("[WS  DEBUG] isConnected=%d  batchCount=%d  payloadBytes=%d\n",
                      isConnected, batchCount, pos);

        if (isConnected) {
          bool ok = webSocket.sendTXT(batchBuffer);
          Serial.printf("[WS  DEBUG] sendTXT -> %s\n", ok ? "OK" : "FAILED");
          if (ok) {
            samplesSent += batchCount;
            digitalWrite(ONBOARD_LED, HIGH);
          } else {
            wsSendFails++;
          }
        } else {
          Serial.println("[WS  DEBUG] skipped send - not connected");
        }

        pos = 0;
        batchBuffer[pos++] = '[';
        batchCount = 0;
      }
    } else {
      checkWifi();
    }
  }
}

// ---------------------------------------------------------------------------
//  Status task  (Core 1, low priority)
//  Prints a concise one-line rate report every STATUS_INTERVAL_S seconds.
//  Target: ~100 samples/sec acquired, ~100 samples/sec sent.
// ---------------------------------------------------------------------------
void statusTask(void* parameter) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_S * 1000));

    uint32_t acq     = samplesAcquired;
    uint32_t sent    = samplesSent;
    uint32_t dropped = samplesDropped;
    uint32_t fails   = wsSendFails;

    // Reset counters
    samplesAcquired = samplesSent = samplesDropped = wsSendFails = 0;

    uint32_t acqRate  = acq  / STATUS_INTERVAL_S;
    uint32_t sentRate = sent / STATUS_INTERVAL_S;

    Serial.printf("[ADC] %3lu Hz acquired | %3lu Hz sent | dropped %lu | WS fails %lu | queue %u\n",
                  acqRate, sentRate, dropped, fails,
                  uxQueueMessagesWaiting(sampleQueue));
  }
}

// ---------------------------------------------------------------------------
//  setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(ONBOARD_LED,     OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  delay(300);

  prefs.begin(NVS_NS, true);
  String savedSSID = prefs.getString(NVS_KEY_SSID, "");
  String savedPass = prefs.getString(NVS_KEY_PASS, "");
  String savedHost = prefs.getString(NVS_KEY_HOST, "");
  prefs.end();

  if (savedSSID.length() == 0 || savedHost.length() == 0) {
    Serial.println("[Setup] Entering provisioning mode...");
    runProvisioningMode(); // never returns
  }

  Serial.printf("[Setup] SSID: %s | Host: %s\n", savedSSID.c_str(), savedHost.c_str());

  if (!connectWiFi(savedSSID, savedPass)) {
    Serial.println("[Setup] WiFi failed - clearing NVS, re-provisioning.");
    prefs.begin(NVS_NS, false); prefs.clear(); prefs.end();
    delay(500);
    runProvisioningMode(); // never returns
  }

  // WebSocket
  webSocket.begin(savedHost.c_str(), WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  Serial.printf("[Setup] WebSocket -> ws://%s:%d%s\n", savedHost.c_str(), WS_PORT, WS_PATH);

  // Queue and tasks
  // ── ADC init (must be in setup, before tasks start) ─────────────────────
  analogReadResolution(12);                        // 12-bit: values 0-4095
  analogSetPinAttenuation(ADC_PIN, ADC_11db);      // 0-3.3 V input range
  pinMode(ADC_PIN, INPUT);                         // ensure input mode

  // ── ADC self-test: 50 reads, print min / max / avg ───────────────────────
  // Tells you immediately whether the ADC pin is alive before tasks start.
  // If all values are ~0 or all identical -> check wiring / shared GND.
  Serial.printf("[ADC Self-Test] Reading GPIO%d 50x...\n", ADC_PIN);
  uint32_t sumVal = 0;
  uint16_t minVal = 4095, maxVal = 0;
  for (int i = 0; i < 50; i++) {
    uint16_t v = (uint16_t)analogRead(ADC_PIN);
    sumVal += v;
    if (v < minVal) minVal = v;
    if (v > maxVal) maxVal = v;
    delay(10);
  }
  Serial.printf("[ADC Self-Test] min=%u  max=%u  avg=%u  (%.3f V avg)\n",
                minVal, maxVal, sumVal / 50,
                (sumVal / 50) * 3.3f / 4095.0f);
  if (maxVal < 10)
    Serial.println("[ADC Self-Test] WARNING: all reads near 0 - check wiring/GND!");
  else if (minVal == maxVal)
    Serial.println("[ADC Self-Test] WARNING: values completely static - pin may be floating.");
  else
    Serial.println("[ADC Self-Test] OK: ADC shows variation.");

  sampleQueue = xQueueCreate(QUEUE_DEPTH, MAX_SAMPLE_STR * sizeof(char));

  xTaskCreatePinnedToCore(adcSampleTask, "ADCSample", 4096, NULL, 3, &adcTask_h,    0);
  xTaskCreatePinnedToCore(sendWSTask,    "WSSend",    8192, NULL, 2, &sendWSTask_h, 1);
  xTaskCreatePinnedToCore(statusTask,    "Status",    4096, NULL, 1, &statusTask_h, 1);

  Serial.printf("[Setup] Sampling GPIO%d (VN) at %d Hz. Streaming...\n",
                ADC_PIN, SAMPLE_RATE_HZ);
}

// ---------------------------------------------------------------------------
//  loop()
//  Monitors the BOOT button. If held for 3 seconds, clears WiFi credentials
//  and restarts into provisioning mode.
// ---------------------------------------------------------------------------
void loop() {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    uint32_t pressTime = millis();
    while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      if (millis() - pressTime > 3000) {
        Serial.println("\n[!] BOOT button held for 3 seconds.");
        Serial.println("[!] Clearing WiFi credentials...");
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
        Serial.println("[!] Rebooting into Provisioning Mode...");
        delay(1000);
        ESP.restart();
      }
      delay(10);
    }
  }
  vTaskDelay(pdMS_TO_TICKS(100)); // check 10 times a second
}
