#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DHT.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

const char *ssid = "";
const char *password = "";

const short DHT11_pin = 2; // D4 / GPIO2
#define DHT11_type DHT11

DHT dht(DHT11_pin, DHT11_type);

float temperature = 0;
float humidity = 0;
unsigned long last_sensor_time = 0;
unsigned long last_log_time = 0;

// Інтервал логування: кожні 60 секунд (60000 мс)
const unsigned long LOG_INTERVAL = 60000;
// Максимальний розмір CSV файлу (30 КБ)
const size_t MAX_LOG_SIZE = 30000;

enum States
{
  CONNECTING,
  SUCCESS,
  FAILED
};

States cur_state = CONNECTING;
ESP8266WebServer server(80);

unsigned long last_time_connected = 0;
unsigned long last_time_failed = 0;
unsigned long start_attempt_time = 0;

// Функція безпечного логування з ротацією
void logDataToCSV(float t, float h)
{
  if (LittleFS.exists("/datalog.csv"))
  {
    File checkFile = LittleFS.open("/datalog.csv", "r");
    if (checkFile)
    {
      size_t fileSize = checkFile.size();
      checkFile.close();

      if (fileSize >= MAX_LOG_SIZE)
      {
        Serial.println("Файл логів досяг ліміту. Ротація...");
        if (LittleFS.exists("/datalog_old.csv"))
        {
          LittleFS.remove("/datalog_old.csv");
        }
        LittleFS.rename("/datalog.csv", "/datalog_old.csv");
      }
    }
  }

  File file = LittleFS.open("/datalog.csv", "a");
  if (!file)
  {
    Serial.println("Помилка відкриття файлу логів!");
    return;
  }

  if (file.size() == 0)
  {
    file.println("uptime_sec,temperature,humidity");
  }

  unsigned long uptime = millis() / 1000;
  file.printf("%lu,%.2f,%.2f\n", uptime, t, h);
  file.close();
  Serial.println("Дані залогітовано у /datalog.csv");
}

void setup()
{
  Serial.begin(115200);
  Serial.print("Connecting to WiFi");

  if (!LittleFS.begin())
  {
    Serial.println("\nПомилка монтування LittleFS, форматуємо...");
    LittleFS.format();
    LittleFS.begin();
  }
  else
  {
    Serial.println("\nLittleFS замонтовано успішно.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  dht.begin();
  start_attempt_time = millis();
}

void loop()
{
  unsigned long cur_time = millis();
  yield(); // Годуємо WDT

  switch (cur_state)
  {
  case CONNECTING:
    if (WiFi.status() == WL_CONNECTED)
    {
      cur_state = SUCCESS;
      Serial.println("\nWiFi connected success!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      // 1. GET API: Телеметрія
      server.on("/api/data", HTTP_GET, []()
                {
        JsonDocument doc;
        doc["uptime"] = millis() / 1000;
        doc["temperature"] = temperature;
        doc["humidity"] = humidity;
        doc["status"] = "OK";
        
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json); });

      // 2. GET API: Список файлів LittleFS
      server.on("/api/files", HTTP_GET, []()
                {
        JsonDocument doc;
        JsonArray filesArray = doc.to<JsonArray>();

        Dir dir = LittleFS.openDir("");
        while (dir.next()) {
          JsonObject fileObj = filesArray.add<JsonObject>();
          fileObj["name"] = dir.fileName();
          fileObj["size"] = dir.fileSize();
        }

        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json); });

      // 3. DELETE API: Видалення файлу
      server.on("/api/file", HTTP_DELETE, []()
                {
        if (!server.hasArg("name")) {
          server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Не вказано назву файлу\"}");
          return;
        }

        String fileName = server.arg("name");
        
        // Захист системних файлів
        if (fileName == "/index.html" || fileName == "/style.css" || fileName == "/app.js") {
          server.send(403, "application/json", "{\"status\":\"error\", \"message\":\"Заборонено видаляти системні файли\"}");
          return;
        }

        if (LittleFS.exists(fileName)) {
          if (LittleFS.remove(fileName)) {
            server.send(200, "application/json", "{\"status\":\"success\", \"deleted\":\"" + fileName + "\"}");
          } else {
            server.send(500, "application/json", "{\"status\":\"error\", \"message\":\"Помилка видалення\"}");
          }
        } else {
          server.send(404, "application/json", "{\"status\":\"error\", \"message\":\"Файл не знайдено\"}");
        } });

      // 4. Роздача статичних файлів із LittleFS
      server.serveStatic("/", LittleFS, "/");

      // SPA Fallback
      server.onNotFound([]()
                        {
        File file = LittleFS.open("/index.html", "r");
        if (file) {
          server.streamFile(file, "text/html");
          file.close();
        } else {
          server.send(404, "text/plain", "404: Not Found");
        } });

      server.begin();
      Serial.println("HTTP server started.");
    }
    else
    {
      if (cur_time - last_time_connected >= 300)
      {
        last_time_connected = cur_time;
        Serial.print(".");
      }
      if (cur_time - start_attempt_time >= 30000)
      {
        cur_state = FAILED;
        last_time_failed = cur_time;
      }
    }
    break;

  case SUCCESS:
    server.handleClient();

    // Опитування датчика кожні 5 секунд
    if (cur_time - last_sensor_time >= 5000)
    {
      last_sensor_time = cur_time;
      float h = dht.readHumidity();
      float t = dht.readTemperature();

      if (!isnan(h) && !isnan(t))
      {
        humidity = h;
        temperature = t;
      }
    }

    // Логування в CSV за таймером
    if (cur_time - last_log_time >= LOG_INTERVAL)
    {
      last_log_time = cur_time;
      if (!isnan(humidity) && !isnan(temperature))
      {
        logDataToCSV(temperature, humidity);
      }
    }
    break;

  case FAILED:
    if (cur_time - last_time_failed >= 10000)
    {
      ESP.restart();
    }
    break;

  default:
    break;
  }
}