#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "ConfigFile.h"
#include "ConfigSettings.h"
#include "Network.h"
#include "Web.h"
#include "Sockets.h"
#include "Utils.h"
#include "Somfy.h"
#include "MQTT.h"
#include "GitOTA.h"
#include "Log.h"

ConfigSettings settings;
Web webServer;
SocketEmitter sockEmit;
Network net;
rebootDelay_t rebootDelay;
SomfyShadeController somfy;
MQTTClass mqtt;
GitUpdater git;

uint32_t oldheap = 0;
void setup() {
  Serial.begin(115200);
  LOGI("Startup/Boot....");
  LOGI("Mounting File System...");
  if(LittleFS.begin()) LOGI("File system mounted successfully");
  else LOGW("Error mounting file system");
  // If shades.cfg is missing, do NOT create it automatically here.
  // ShadeConfigFile::load() and Somfy will handle missing files gracefully
  // and creation will occur when saving/committing via the UI or API.
  if(!ShadeConfigFile::exists()) {
    LOGI("shades.cfg not present on filesystem; continuing without creating it");
  }
  settings.begin();
  if(WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);
  delay(10);
  LOGD("");
  webServer.startup();
  webServer.begin();
  delay(1000);
  net.setup();  
  somfy.begin();
  //git.checkForUpdate();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const esp_task_wdt_config_t wdt_config = { .timeout_ms = 15000, .idle_core_mask = 1, .trigger_panic = true };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(15, true); //enable panic so ESP32 restarts
#endif
  esp_task_wdt_add(NULL); //add current thread to WDT watch

}

void loop() {
  // put your main code here, to run repeatedly:
  //uint32_t heap = ESP.getFreeHeap();
  if(rebootDelay.reboot && millis() > rebootDelay.rebootTime) {
    LOGI("Rebooting after %lu ms", (unsigned long)rebootDelay.rebootTime);
    net.end();
    ESP.restart();
    return;
  }
  uint32_t timing = millis();
  
  net.loop();
  if(millis() - timing > 100) LOGD("Timing Net: %ldms", millis() - timing);
  timing = millis();
  esp_task_wdt_reset();
  somfy.loop();
  if(millis() - timing > 100) LOGD("Timing Somfy: %ldms", millis() - timing);
  timing = millis();
  esp_task_wdt_reset();
  if(net.connected() || net.softAPOpened) {
    if(!rebootDelay.reboot && net.connected() && !net.softAPOpened) {
      git.loop();
      esp_task_wdt_reset();
    }
    webServer.loop();
    esp_task_wdt_reset();
    if(millis() - timing > 100) LOGD("Timing WebServer: %ldms", millis() - timing);
    esp_task_wdt_reset();
    timing = millis();
    sockEmit.loop();
    if(millis() - timing > 100) LOGD("Timing Socket: %ldms", millis() - timing);
    esp_task_wdt_reset();
    timing = millis();
  }
  if(rebootDelay.reboot && millis() > rebootDelay.rebootTime) {
    net.end();
    ESP.restart();
  }
  esp_task_wdt_reset();
}
