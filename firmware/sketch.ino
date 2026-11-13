#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_system.h>
// ===== Kieu du lieu va thuat toan (da gop tu file .h) =====
#include <math.h>
#include <stdint.h>

struct Sample {
  uint32_t ms, seq;
  float ax, ay, az, totalG, roll, pitch, tilt;
  bool valid;
};
struct Command { char name[16], id[64]; bool value; };
struct OutputState {
  bool manual, led, buzzer, fall, candidate, sensorOk;
  char ack[64];
};

// Thuat toan minh hoa cho Lab, cac nguong can hieu chinh khi dung thuc te.
class FallDetector {
 public:
  bool latched = false, candidate = false;
  void reset() { latched = candidate = ready = holding = false; }
  void update(const Sample &s) {
    if (!s.valid) { candidate = holding = ready = false; return; }
    if (latched) return;
    if (!ready) {
      // Chi lay moc tu the khi gia toc gan 1g.
      if (s.totalG < 0.7f || s.totalG > 1.3f) return;
      bx = s.ax; by = s.ay; bz = s.az; previousG = s.totalG;
      ready = true;
      return;
    }
    bool shock = s.totalG >= 1.8f || s.totalG <= 0.45f ||
                 fabsf(s.totalG - previousG) >= 0.65f;
    if (!candidate && shock) {
      candidate = true; started = s.ms; holding = false;
    }
    if (candidate) {
      if ((uint32_t)(s.ms - started) > 5000) {
        candidate = holding = false;
      } else {
        float denom = sqrtf(bx*bx + by*by + bz*bz) * s.totalG * 9.80665f;
        float cosine = denom > 0.01f ? (bx*s.ax + by*s.ay + bz*s.az)/denom : 1;
        cosine = fmaxf(-1, fminf(1, cosine));
        float change = acosf(cosine) * 57.2957795f;
        bool posture = s.totalG >= 0.7f && s.totalG <= 1.3f &&
                       s.tilt >= 55 && change >= 35;
        if (!posture) holding = false;
        else if (!holding) { holding = true; heldSince = s.ms; }
        else if ((uint32_t)(s.ms - heldSince) >= 400) {
          latched = true; candidate = false;
        }
      }
    }
    // Cap nhat moc cham, KHONG doi moc trong cua so xac nhan te nga.
    if (!candidate && !latched && s.totalG >= 0.7f && s.totalG <= 1.3f) {
      bx = 0.96f*bx + 0.04f*s.ax;
      by = 0.96f*by + 0.04f*s.ay;
      bz = 0.96f*bz + 0.04f*s.az;
    }
    previousG = s.totalG;
  }
 private:
  bool ready = false, holding = false;
  float bx = 0, by = 0, bz = 9.80665f, previousG = 1;
  uint32_t started = 0, heldSince = 0;
};

// ===== Chuong trinh ESP32 =====

constexpr int LED_PIN = 18, BUZZER_PIN = 19;
const char *BROKER = "broker.hivemq.com";
// Giong TOPIC_BASE trong index.html. Doi ca hai neu chay nhieu bo.
const char *TOPIC_BASE = "utc/lovis/de5/fall/a73c91";
char topicData[100], topicState[100], topicCommand[100], topicStatus[100];
char bootId[16], clientId[40];
Adafruit_MPU6050 mpu;
WiFiClient net;
PubSubClient mqtt(net);
QueueHandle_t dataQueue, dataAlert, commandQueue;
SemaphoreHandle_t stateMutex;
OutputState output = {};

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, topicCommand) != 0 || length > 256) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  // Lenh chi co gia tri voi phien khoi dong hien tai.
  if (strcmp(doc["boot"] | "", bootId) != 0) return;
  const char *name = doc["command"] | "";
  const char *id = doc["id"] | "";
  if (!*id || strlen(id) >= 64) return;
  if (strcmp(name,"mode") && strcmp(name,"led") &&
      strcmp(name,"buzzer") && strcmp(name,"reset")) return;
  if (strcmp(name,"reset") && !doc["value"].is<bool>()) return;
  Command cmd = {};
  strlcpy(cmd.name, name, sizeof(cmd.name));
  strlcpy(cmd.id, id, sizeof(cmd.id));
  cmd.value = doc["value"] | false;
  // Khong block task mang. Web se bao timeout neu hang doi day.
  xQueueSend(commandQueue, &cmd, 0);
}

void TaskRead(void *) {
  Wire.begin(21, 22);
  Wire.setTimeOut(50);
  bool sensorReady = false;
  uint32_t seq = 0, retryAt = 0, serialAt = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    uint32_t now = millis();
    if (!sensorReady && (retryAt == 0 || now - retryAt >= 2000)) {
      retryAt = now;
      sensorReady = mpu.begin(0x68);
      if (sensorReady) {
        mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
        Serial.println("MPU6050 ready, Core 1");
      } else Serial.println("MPU6050 missing: check SDA21/SCL22/3V3/GND");
    }
    Sample s = {};
    s.ms = now; s.seq = ++seq;
    if (sensorReady) {
      // Kiem tra ACK I2C de khong hien du lieu cu khi rut cam bien.
      Wire.beginTransmission(0x68);
      if (Wire.endTransmission() != 0) sensorReady = false;
      else {
        sensors_event_t a = {}, g = {}, t = {};
        s.valid = mpu.getEvent(&a, &g, &t);
        s.ax = a.acceleration.x; s.ay = a.acceleration.y; s.az = a.acceleration.z;
        float magnitude = sqrtf(s.ax*s.ax + s.ay*s.ay + s.az*s.az);
        s.totalG = magnitude / 9.80665f;
        s.roll = atan2f(s.ay, s.az) * 57.2957795f;
        s.pitch = atan2f(-s.ax, sqrtf(s.ay*s.ay + s.az*s.az)) * 57.2957795f;
        s.tilt = magnitude > 0.1f ? acosf(fmaxf(-1, fminf(1, s.az/magnitude))) * 57.2957795f : 0;
        s.valid = s.valid && isfinite(s.totalG) && isfinite(s.tilt);
      }
    }
    xQueueOverwrite(dataQueue, &s); // Queue 1 phan tu: mang luon lay mau moi.
    if (xQueueSend(dataAlert, &s, 0) != pdPASS) {
      // FIFO day: bo mau cu nhat, giu mau moi; khong block task doc.
      Sample old;
      xQueueReceive(dataAlert, &old, 0);
      xQueueSend(dataAlert, &s, 0);
      Serial.println("WARN: dataAlert overflow");
    }
    if (now - serialAt >= 200) {
      serialAt = now;
      Serial.printf("t=%lu Ax=%.2f Ay=%.2f Az=%.2f m/s2 | A=%.2fg Roll=%.1f Pitch=%.1f Tilt=%.1f valid=%d\n",
        (unsigned long)s.ms,s.ax,s.ay,s.az,s.totalG,s.roll,s.pitch,s.tilt,s.valid);
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20)); // 50 Hz
  }
}

void TaskAlert(void *) {
  pinMode(LED_PIN, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); digitalWrite(BUZZER_PIN, LOW);
  FallDetector detector;
  OutputState local = {};
  bool manualLed = false, manualBuzzer = false, oldBuzz = false;
  uint32_t lastSample = 0;
  for (;;) {
    Sample s;
    // Xu ly toi da 32 mau moi vong, tranh command bi doi vo han.
    for (int i = 0; i < 32 && xQueueReceive(dataAlert, &s, 0) == pdTRUE; i++) {
      detector.update(s);
      local.sensorOk = s.valid;
      lastSample = millis();
    }
    if (millis() - lastSample > 1000) local.sensorOk = false;
    Command cmd;
    while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
      if (strcmp(cmd.id, local.ack) == 0) continue;
      if (!strcmp(cmd.name,"mode")) {
        local.manual = cmd.value;
        manualLed = manualBuzzer = false;
      } else if (!strcmp(cmd.name,"led") && local.manual) manualLed = cmd.value;
      else if (!strcmp(cmd.name,"buzzer") && local.manual) manualBuzzer = cmd.value;
      else if (!strcmp(cmd.name,"reset")) detector.reset();
      else continue; // Lenh khong hop le trong mode hien tai: khong ACK.
      strlcpy(local.ack, cmd.id, sizeof(local.ack));
    }
    local.fall = detector.latched; local.candidate = detector.candidate;
    local.led = local.manual ? manualLed : local.fall;
    local.buzzer = local.manual ? manualBuzzer : local.fall;
    digitalWrite(LED_PIN, local.led ? HIGH : LOW);
    if (local.buzzer != oldBuzz) {
      if (local.buzzer) tone(BUZZER_PIN, 2000);
      else noTone(BUZZER_PIN);
      oldBuzz = local.buzzer;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    output = local;
    xSemaphoreGive(stateMutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void TaskTransmit(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", "", 6);
  mqtt.setServer(BROKER, 1883);
  mqtt.setCallback(mqttCallback);
  if (!mqtt.setBufferSize(1024)) { Serial.println("MQTT buffer allocation failed"); vTaskDelete(NULL); }
  mqtt.setKeepAlive(15); mqtt.setSocketTimeout(2);
  uint32_t wifiAt = millis(), mqttAt = 0, publishAt = 0;
  bool triedMqtt = false;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (mqtt.connected()) mqtt.disconnect();
      if (now - wifiAt >= 10000) {
        wifiAt = now; WiFi.disconnect(); WiFi.begin("Wokwi-GUEST", "", 6);
        Serial.println("Retry WiFi...");
      }
    } else if (!mqtt.connected()) {
      if (!triedMqtt || now - mqttAt >= 3000) {
        triedMqtt = true; mqttAt = now;
        if (mqtt.connect(clientId, topicStatus, 1, true, "offline")) {
          if (!mqtt.subscribe(topicCommand)) mqtt.disconnect();
          else {
            mqtt.publish(topicStatus, "online", true);
            Serial.printf("MQTT connected / Core 0 / topic %s\n", TOPIC_BASE);
          }
        } else Serial.printf("MQTT failed rc=%d\n", mqtt.state());
      }
    } else {
      mqtt.loop();
      if (now - publishAt >= 200) {
        publishAt = now;
        Sample s;
        OutputState state;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        state = output;
        xSemaphoreGive(stateMutex);
        char buffer[768];
        if (xQueueReceive(dataQueue, &s, 0) == pdTRUE) {
          JsonDocument doc;
          doc["boot"] = bootId; doc["seq"] = s.seq; doc["ms"] = s.ms;
          doc["valid"] = s.valid;
          doc["ax"] = s.ax; doc["ay"] = s.ay; doc["az"] = s.az;
          doc["total_g"] = s.totalG;
          doc["roll"] = s.roll; doc["pitch"] = s.pitch; doc["tilt"] = s.tilt;
          serializeJson(doc, buffer, sizeof(buffer));
          mqtt.publish(topicData, buffer, false);
        }
        JsonDocument doc;
        doc["boot"] = bootId; doc["mode"] = state.manual ? "MANUAL" : "AUTO";
        doc["led"] = state.led; doc["buzzer"] = state.buzzer;
        doc["fall"] = state.fall; doc["candidate"] = state.candidate;
        doc["sensor_ok"] = state.sensorOk; doc["ack"] = state.ack;
        serializeJson(doc, buffer, sizeof(buffer));
        mqtt.publish(topicState, buffer, false); // Khong hien state cu khi mo web.
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  snprintf(topicData,sizeof(topicData),"%s/data",TOPIC_BASE);
  snprintf(topicState,sizeof(topicState),"%s/state",TOPIC_BASE);
  snprintf(topicCommand,sizeof(topicCommand),"%s/command",TOPIC_BASE);
  snprintf(topicStatus,sizeof(topicStatus),"%s/status",TOPIC_BASE);
  snprintf(bootId,sizeof(bootId),"%08lx",(unsigned long)esp_random());
  snprintf(clientId,sizeof(clientId),"fall-esp32-%s",bootId);
  dataQueue = xQueueCreate(1,sizeof(Sample));
  dataAlert = xQueueCreate(32,sizeof(Sample));
  commandQueue = xQueueCreate(8,sizeof(Command));
  stateMutex = xSemaphoreCreateMutex();
  if (!dataQueue || !dataAlert || !commandQueue || !stateMutex) {
    Serial.println("FATAL: queue/mutex allocation failed");
    while (true) delay(1000);
  }
  // Gia tri stack theo byte tren ESP32. Priority cao hon = uu tien cao hon.
  BaseType_t a = xTaskCreatePinnedToCore(TaskAlert,"Task canh bao",4096,NULL,3,NULL,1);
  BaseType_t r = xTaskCreatePinnedToCore(TaskRead,"Task doc data",4096,NULL,2,NULL,1);
  BaseType_t t = xTaskCreatePinnedToCore(TaskTransmit,"Task truyen data",8192,NULL,1,NULL,0);
  if (a != pdPASS || r != pdPASS || t != pdPASS) {
    Serial.println("FATAL: task allocation failed");
    while (true) delay(1000);
  }
}
void loop() { delay(1000); }
