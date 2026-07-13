#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <DHTesp.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// WiFi
const char* ssid = "Smiles";
const char* password = "JL1826JL";

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1

// DHT
#define DHT_PIN 14

// Peripherals
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_ADS1115 ads;
DHTesp dht;
WebServer server(80);

// Shared sensor data struct
struct SensorData {
  float temperature;
  float humidity;
  float aqVoltage;
};

// FreeRTOS queue and mutex
QueueHandle_t sensorQueue;
SemaphoreHandle_t displayMutex;

// Watchdog task handles
TaskHandle_t sensorTaskHandle;
TaskHandle_t displayTaskHandle;
TaskHandle_t serverTaskHandle;

// Watchdog counters
volatile int sensorCounter = 0;
volatile int displayCounter = 0;
volatile int serverCounter = 0;

// Latest sensor data for web server
SensorData latestData = {0, 0, 0};

// ─── Task 1: Sensor Reading ───────────────────────────────────────────────────
void sensorTask(void* pvParameters) {
  while (true) {
    SensorData data;
    data.temperature = dht.getTemperature();
    data.humidity = dht.getHumidity();
    int16_t raw = ads.readADC_SingleEnded(0);
    data.aqVoltage = raw * 0.0001875;

    // Push to queue, overwrite if full
    if (xQueueSend(sensorQueue, &data, 0) != pdTRUE) {
      xQueueOverwrite(sensorQueue, &data);
    }

    // Update latest for web server
    latestData = data;

    sensorCounter++;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ─── Task 2: Display Update ───────────────────────────────────────────────────
void displayTask(void* pvParameters) {
  SensorData data;
  int screen = 0;

  while (true) {
    if (xQueuePeek(sensorQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);

        switch (screen % 3) {
          case 0:
            display.print("Temp: ");
            display.print(data.temperature, 1);
            display.println(" C");
            break;
          case 1:
            display.print("Humidity: ");
            display.print(data.humidity, 1);
            display.println("%");
            break;
          case 2:
            display.print("AQ: ");
            display.print(data.aqVoltage, 2);
            display.println(" V");
            break;
        }

        display.display();
        xSemaphoreGive(displayMutex);
        screen++;
      }
    }

    displayCounter++;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ─── Task 3: WiFi Web Server ──────────────────────────────────────────────────
void serverTask(void* pvParameters) {
  while (true) {
    server.handleClient();
    serverCounter++;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── Task 4: Watchdog ─────────────────────────────────────────────────────────
void watchdogTask(void* pvParameters) {
  int lastSensor = 0, lastDisplay = 0, lastServer = 0;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (sensorCounter == lastSensor) {
      Serial.println("WATCHDOG: Sensor task hung, restarting...");
      ESP.restart();
    }
    if (displayCounter == lastDisplay) {
      Serial.println("WATCHDOG: Display task hung, restarting...");
      ESP.restart();
    }
    if (serverCounter == lastServer) {
      Serial.println("WATCHDOG: Server task hung, restarting...");
      ESP.restart();
    }

    lastSensor = sensorCounter;
    lastDisplay = displayCounter;
    lastServer = serverCounter;

    Serial.println("Watchdog OK");
  }
}

// ─── Web Server Handler ───────────────────────────────────────────────────────
void handleRoot() {
  String html = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta http-equiv='refresh' content='3'>
  <title>ESP32 Environmental Monitor</title>
  <style>
    body { background: #0a0a0a; color: #00ff88; font-family: monospace; display: flex; flex-direction: column; align-items: center; justify-content: center; height: 100vh; margin: 0; }
    h1 { letter-spacing: 4px; font-size: 1.2rem; }
    .card { background: #111; border: 1px solid #00ff8833; border-radius: 8px; padding: 20px 40px; margin: 10px; min-width: 200px; text-align: center; }
    .value { font-size: 2rem; color: #00ccff; }
    .label { font-size: 0.8rem; opacity: 0.6; margin-top: 4px; }
    .grid { display: flex; gap: 20px; }
  </style>
</head>
<body>
  <h1>ESP32 ENVIRONMENTAL MONITOR</h1>
  <div class='grid'>
    <div class='card'><div class='value'>TEMP_VAL &deg;C</div><div class='label'>TEMPERATURE</div></div>
    <div class='card'><div class='value'>HUM_VAL %</div><div class='label'>HUMIDITY</div></div>
    <div class='card'><div class='value'>AQ_VAL V</div><div class='label'>AIR QUALITY</div></div>
  </div>
</body>
</html>
)";

  html.replace("TEMP_VAL", String(latestData.temperature, 1));
  html.replace("HUM_VAL", String(latestData.humidity, 1));
  html.replace("AQ_VAL", String(latestData.aqVoltage, 2));

  server.send(200, "text/html", html);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  delay(2000);
  Serial.begin(115200);
  Wire.begin();

  dht.setup(DHT_PIN, DHTesp::DHT22);

  if (!ads.begin()) {
    Serial.println("ADS1115 not found!");
    while (1);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("IP:");
  display.print(WiFi.localIP());
  display.display();
  delay(2000);

  server.on("/", handleRoot);
  server.begin();

  // Create queue and mutex
  sensorQueue = xQueueCreate(1, sizeof(SensorData));
  displayMutex = xSemaphoreCreateMutex();

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(sensorTask,   "SensorTask",   4096, NULL, 3, &sensorTaskHandle,  1);
  xTaskCreatePinnedToCore(displayTask,  "DisplayTask",  4096, NULL, 2, &displayTaskHandle, 1);
  xTaskCreatePinnedToCore(serverTask,   "ServerTask",   8192, NULL, 1, &serverTaskHandle,  1);
  xTaskCreatePinnedToCore(watchdogTask, "WatchdogTask", 2048, NULL, 1, NULL,               1);
}

void loop() {
  // Empty — FreeRTOS handles everything
}