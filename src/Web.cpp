#include <WiFi.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "mbedtls/md.h"
#include "ConfigSettings.h"
#include "ConfigFile.h"
#include "Utils.h"
#include "SSDP.h"
#include "Somfy.h"
#include "WResp.h"
#include "Web.h"
#include "MQTT.h"
#include "GitOTA.h"
#include "Network.h"
#include "Log.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>

#define LOG_TAG "Web"

extern ConfigSettings settings;
extern SSDPClass SSDP;
extern rebootDelay_t rebootDelay;
extern SomfyShadeController somfy;
extern Web webServer;
extern MQTTClass mqtt;
extern GitUpdater git;
extern Network net;

#define WEB_MAX_RESPONSE 4096
static char g_content[WEB_MAX_RESPONSE];

static const char _response_404[] = "404: Service Not Found";
static const char _encoding_text[] = "text/plain";
static const char _encoding_html[] = "text/html";
static const char _encoding_json[] = "application/json";

static QueueHandle_t webCmdQueue = nullptr;
static SemaphoreHandle_t webCmdDone = nullptr;

AsyncWebServer asyncServer(80);
AsyncWebServer asyncApiServer(8081);

// Helper functions
static String asyncParam(AsyncWebServerRequest *request, const char *name) {
  if(request->hasParam(name)) return request->getParam(name)->value();
  return String();
}
static bool asyncHasParam(AsyncWebServerRequest *request, const char *name) {
  return request->hasParam(name);
}

void Web::startup() {
  LOGI("Launching web server...");
  if(!webCmdQueue) webCmdQueue = xQueueCreate(WEB_CMD_QUEUE_SIZE, sizeof(web_command_t));
  if(!webCmdDone) webCmdDone = xSemaphoreCreateBinary();
  asyncApiServer.begin();
  LOGI("Async API server started on port 8081");
}
void Web::loop() {
  this->processQueue();
  delay(1);
}
void Web::end() {
}
bool Web::queueCommand(const web_command_t &cmd) {
  if(!webCmdQueue || !webCmdDone) return false;
  xSemaphoreTake(webCmdDone, 0);
  if(xQueueSend(webCmdQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    LOGE("Command queue full, dropping command");
    return false;
  }
  if(xSemaphoreTake(webCmdDone, pdMS_TO_TICKS(WEB_CMD_TIMEOUT_MS)) != pdTRUE) {
    LOGW("Command queue timeout waiting for processing");
    return false;
  }
  return true;
}
void Web::processQueue() {
  if(!webCmdQueue || !webCmdDone) return;
  web_command_t cmd;
  while(xQueueReceive(webCmdQueue, &cmd, 0) == pdTRUE) {
    switch(cmd.type) {
      case web_cmd_t::shade_command: {
        SomfyShade *shade = somfy.getShadeById(cmd.shadeId);
        if(shade) {
          if(cmd.target <= 100) shade->moveToTarget(shade->transformPosition(cmd.target));
          else shade->sendCommand(cmd.command, cmd.repeat > 0 ? cmd.repeat : shade->repeats, cmd.stepSize);
        }
        break;
      }
      case web_cmd_t::group_command: {
        SomfyGroup *group = somfy.getGroupById(cmd.groupId);
        if(group) group->sendCommand(cmd.command, cmd.repeat >= 0 ? cmd.repeat : group->repeats, cmd.stepSize);
        break;
      }
      case web_cmd_t::tilt_command: {
        SomfyShade *shade = somfy.getShadeById(cmd.shadeId);
        if(shade) {
          if(cmd.target <= 100) shade->moveToTiltTarget(shade->transformPosition(cmd.target));
          else shade->sendTiltCommand(cmd.command);
        }
        break;
      }
      case web_cmd_t::shade_repeat: {
        SomfyShade *shade = somfy.getShadeById(cmd.shadeId);
        if(shade) {
          if(shade->shadeType == shade_types::garage1 && cmd.command == somfy_commands::Prog) cmd.command = somfy_commands::Toggle;
          if(!shade->isLastCommand(cmd.command)) shade->sendCommand(cmd.command, cmd.repeat >= 0 ? cmd.repeat : shade->repeats, cmd.stepSize);
          else shade->repeatFrame(cmd.repeat >= 0 ? cmd.repeat : shade->repeats);
        }
        break;
      }
      case web_cmd_t::group_repeat: {
        SomfyGroup *group = somfy.getGroupById(cmd.groupId);
        if(group) {
          if(!group->isLastCommand(cmd.command)) group->sendCommand(cmd.command, cmd.repeat >= 0 ? cmd.repeat : group->repeats, cmd.stepSize);
          else group->repeatFrame(cmd.repeat >= 0 ? cmd.repeat : group->repeats);
        }
        break;
      }
      case web_cmd_t::set_positions: {
        SomfyShade *shade = somfy.getShadeById(cmd.shadeId);
        if(shade) {
          if(cmd.position >= 0) shade->target = shade->currentPos = cmd.position;
          if(cmd.tiltPosition >= 0 && shade->tiltType != tilt_types::none) shade->tiltTarget = shade->currentTiltPos = cmd.tiltPosition;
          shade->emitState();
        }
        break;
      }
      case web_cmd_t::shade_sensor: {
        SomfyShade *shade = somfy.getShadeById(cmd.shadeId);
        if(shade) {
          shade->sendSensorCommand(cmd.windy, cmd.sunny, cmd.repeat >= 0 ? (uint8_t)cmd.repeat : shade->repeats);
          shade->emitState();
        }
        break;
      }
      case web_cmd_t::group_sensor: {
        SomfyGroup *group = somfy.getGroupById(cmd.groupId);
        if(group) {
          group->sendSensorCommand(cmd.windy, cmd.sunny, cmd.repeat >= 0 ? (uint8_t)cmd.repeat : group->repeats);
          group->emitState();
        }
        break;
      }
    }
    xSemaphoreGive(webCmdDone);
  }
}
bool Web::isAuthenticated(AsyncWebServerRequest *request, bool cfg) {
  LOGD("Checking authentication");
  if(settings.Security.type == security_types::None) return true;
  else if(!cfg && (settings.Security.permissions & static_cast<uint8_t>(security_permissions::ConfigOnly)) == 0x01) return true;
  else if(request->hasHeader("apikey")) {
    LOGD("Checking API Key...");
    char token[65];
    memset(token, 0x00, sizeof(token));
    this->createAPIToken(request->client()->remoteIP(), token);
    if(String(token) != request->getHeader("apikey")->value()) {
      request->send(401, _encoding_text, "Unauthorized API Key");
      return false;
    }
  }
  else {
    LOGW("Not authenticated...");
    request->send(401, _encoding_text, "Unauthorized API Key");
    return false;
  }
  return true;
}
bool Web::createAPIPinToken(const IPAddress ipAddress, const char *pin, char *token) {
  return this->createAPIToken((String(pin) + ":" + ipAddress.toString()).c_str(), token);
}
bool Web::createAPIPasswordToken(const IPAddress ipAddress, const char *username, const char *password, char *token) {
  return this->createAPIToken((String(username) + ":" + String(password) + ":" + ipAddress.toString()).c_str(), token);
}
bool Web::createAPIToken(const char *payload, char *token) {
    byte hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)settings.serverId, strlen(settings.serverId));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)payload, strlen(payload));
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);
    token[0] = '\0';
    for(int i = 0; i < (int)sizeof(hmacResult); i++){
        char str[3];
        sprintf(str, "%02x", (int)hmacResult[i]);
        strcat(token, str);
    }
    return true;
}
bool Web::createAPIToken(const IPAddress ipAddress, char *token) {
    if(settings.Security.type == security_types::Password) createAPIPasswordToken(ipAddress, settings.Security.username, settings.Security.password, token);
    else if(settings.Security.type == security_types::PinEntry) createAPIPinToken(ipAddress, settings.Security.pin, token);
    else createAPIToken(ipAddress.toString().c_str(), token);
    return true;
}

// =====================================================
// Async API Handlers
// =====================================================
void Web::handleDiscovery(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    LOGI("Discovery Requested");
    char connType[10] = "Unknown";
    if(net.connType == conn_types_t::ethernet) strcpy(connType, "Ethernet");
    else if(net.connType == conn_types_t::wifi) strcpy(connType, "Wifi");
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("serverId", settings.serverId);
    resp.addElem("version", settings.fwVersion.name);
    resp.addElem("latest", git.latest.name);
    resp.addElem("model", "ESPSomfyRTS");
    resp.addElem("hostname", settings.hostname);
    resp.addElem("authType", static_cast<uint8_t>(settings.Security.type));
    resp.addElem("permissions", settings.Security.permissions);
    resp.addElem("chipModel", settings.chipModel);
    resp.addElem("connType", connType);
    resp.addElem("checkForUpdate", settings.checkForUpdate);
    resp.beginObject("memory");
    resp.addElem("max", ESP.getMaxAllocHeap());
    resp.addElem("free", ESP.getFreeHeap());
    resp.addElem("min", ESP.getMinFreeHeap());
    resp.addElem("total", ESP.getHeapSize());
    resp.endObject();
    resp.beginArray("rooms");
    somfy.toJSONRooms(resp);
    resp.endArray();
    resp.beginArray("shades");
    somfy.toJSONShades(resp);
    resp.endArray();
    resp.beginArray("groups");
    somfy.toJSONGroups(resp);
    resp.endArray();
    resp.endObject();
    resp.endResponse();
    net.needsBroadcast = true;
  }
  else request->send(500, _encoding_text, "Invalid http method");
}
void Web::handleGetRooms(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginArray();
    somfy.toJSONRooms(resp);
    resp.endArray();
    resp.endResponse();
  }
  else request->send(404, _encoding_text, _response_404);
}
void Web::handleGetShades(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginArray();
    somfy.toJSONShades(resp);
    resp.endArray();
    resp.endResponse();
  }
  else request->send(404, _encoding_text, _response_404);
}
void Web::handleGetGroups(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginArray();
    somfy.toJSONGroups(resp);
    resp.endArray();
    resp.endResponse();
  }
  else request->send(404, _encoding_text, _response_404);
}
void Web::handleGetRepeaters(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginArray();
    somfy.toJSONRepeaters(resp);
    resp.endArray();
    resp.endResponse();
  }
  else request->send(404, _encoding_text, _response_404);
}
void Web::handleController(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_POST || request->method() == HTTP_GET) {
    settings.printAvailHeap();
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("maxRooms", (uint8_t)SOMFY_MAX_ROOMS);
    resp.addElem("maxShades", (uint8_t)SOMFY_MAX_SHADES);
    resp.addElem("maxGroups", (uint8_t)SOMFY_MAX_GROUPS);
    resp.addElem("maxGroupedShades", (uint8_t)SOMFY_MAX_GROUPED_SHADES);
    resp.addElem("maxLinkedRemotes", (uint8_t)SOMFY_MAX_LINKED_REMOTES);
    resp.addElem("startingAddress", (uint32_t)somfy.startingAddress);
    resp.beginObject("transceiver");
    somfy.transceiver.toJSON(resp);
    resp.endObject();
    resp.beginObject("version");
    git.toJSON(resp);
    resp.endObject();
    resp.beginArray("rooms");
    somfy.toJSONRooms(resp);
    resp.endArray();
    resp.beginArray("shades");
    somfy.toJSONShades(resp);
    resp.endArray();
    resp.beginArray("groups");
    somfy.toJSONGroups(resp);
    resp.endArray();
    resp.beginArray("repeaters");
    somfy.toJSONRepeaters(resp);
    resp.endArray();
    resp.endObject();
    resp.endResponse();
  }
  else request->send(404, _encoding_text, _response_404);
}
void Web::handleLogin(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  char token[65];
  memset(&token, 0x00, sizeof(token));
  this->createAPIToken(request->client()->remoteIP(), token);
  if(settings.Security.type == security_types::None) {
    snprintf(g_content, sizeof(g_content),
      "{\"type\":%u,\"apiKey\":\"%s\",\"msg\":\"Success\",\"success\":true}",
      static_cast<uint8_t>(settings.Security.type), token);
    request->send(200, _encoding_json, g_content);
    return;
  }
  char username[33] = "";
  char password[33] = "";
  char pin[5] = "";
  if(asyncHasParam(request, "username")) strlcpy(username, asyncParam(request, "username").c_str(), sizeof(username));
  if(asyncHasParam(request, "password")) strlcpy(password, asyncParam(request, "password").c_str(), sizeof(password));
  if(asyncHasParam(request, "pin")) strlcpy(pin, asyncParam(request, "pin").c_str(), sizeof(pin));
  if(!json.isNull()) {
    DynamicJsonDocument docin(512);
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("username") && obj["username"]) strlcpy(username, obj["username"], sizeof(username));
    if(obj.containsKey("password") && obj["password"]) strlcpy(password, obj["password"], sizeof(password));
    if(obj.containsKey("pin") && obj["pin"]) strlcpy(pin, obj["pin"], sizeof(pin));
  }
  bool success = false;
  const char *msg = "Invalid credentials";
  if(settings.Security.type == security_types::PinEntry) {
    if(strlen(pin) > 0 && strcmp(pin, settings.Security.pin) == 0) success = true;
    else msg = "Invalid Pin Entry";
  }
  else if(settings.Security.type == security_types::Password) {
    if(strlen(username) > 0 && strlen(password) > 0 &&
       strcmp(username, settings.Security.username) == 0 &&
       strcmp(password, settings.Security.password) == 0) success = true;
    else msg = "Invalid username or password";
  }
  if(success) {
    snprintf(g_content, sizeof(g_content),
      "{\"type\":%u,\"apiKey\":\"%s\",\"msg\":\"Login successful\",\"success\":true}",
      static_cast<uint8_t>(settings.Security.type), token);
    request->send(200, _encoding_json, g_content);
  } else {
    snprintf(g_content, sizeof(g_content),
      "{\"type\":%u,\"msg\":\"%s\",\"success\":false}",
      static_cast<uint8_t>(settings.Security.type), msg);
    request->send(200, _encoding_json, g_content);
  }
}
void Web::handleShadeCommand(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(!this->isAuthenticated(request)) return;
  uint8_t shadeId = 255;
  uint8_t target = 255;
  uint8_t stepSize = 0;
  int8_t repeat = -1;
  somfy_commands command = somfy_commands::My;
  if(asyncHasParam(request, "shadeId")) {
    shadeId = asyncParam(request, "shadeId").toInt();
    if(asyncHasParam(request, "command")) command = translateSomfyCommand(asyncParam(request, "command"));
    else if(asyncHasParam(request, "target")) target = asyncParam(request, "target").toInt();
    if(asyncHasParam(request, "repeat")) repeat = asyncParam(request, "repeat").toInt();
    if(asyncHasParam(request, "stepSize")) stepSize = asyncParam(request, "stepSize").toInt();
  }
  else if(!json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) shadeId = obj["shadeId"];
    else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade id was supplied.\"}")); return; }
    if(obj.containsKey("command")) { String scmd = obj["command"]; command = translateSomfyCommand(scmd); }
    else if(obj.containsKey("target")) target = obj["target"].as<uint8_t>();
    if(obj.containsKey("repeat")) repeat = obj["repeat"].as<uint8_t>();
    if(obj.containsKey("stepSize")) stepSize = obj["stepSize"].as<uint8_t>();
  }
  else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade object supplied.\"}")); return; }
  SomfyShade *shade = somfy.getShadeById(shadeId);
  if(shade) {
    web_command_t cmd = {};
    cmd.type = web_cmd_t::shade_command;
    cmd.shadeId = shadeId;
    cmd.target = target;
    cmd.command = command;
    cmd.repeat = repeat;
    cmd.stepSize = stepSize;
    this->queueCommand(cmd);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    shade->toJSONRef(resp);
    resp.endObject();
    resp.endResponse();
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade with the specified id not found.\"}"));
}
void Web::handleGroupCommand(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(!this->isAuthenticated(request)) return;
  uint8_t groupId = 255;
  uint8_t stepSize = 0;
  int8_t repeat = -1;
  somfy_commands command = somfy_commands::My;
  if(asyncHasParam(request, "groupId")) {
    groupId = asyncParam(request, "groupId").toInt();
    if(asyncHasParam(request, "command")) command = translateSomfyCommand(asyncParam(request, "command"));
    if(asyncHasParam(request, "repeat")) repeat = asyncParam(request, "repeat").toInt();
    if(asyncHasParam(request, "stepSize")) stepSize = asyncParam(request, "stepSize").toInt();
  }
  else if(!json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("groupId")) groupId = obj["groupId"];
    else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No group id was supplied.\"}")); return; }
    if(obj.containsKey("command")) { String scmd = obj["command"]; command = translateSomfyCommand(scmd); }
    if(obj.containsKey("repeat")) repeat = obj["repeat"].as<uint8_t>();
    if(obj.containsKey("stepSize")) stepSize = obj["stepSize"].as<uint8_t>();
  }
  else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No group object supplied.\"}")); return; }
  SomfyGroup *group = somfy.getGroupById(groupId);
  if(group) {
    web_command_t cmd = {};
    cmd.type = web_cmd_t::group_command;
    cmd.groupId = groupId;
    cmd.command = command;
    cmd.repeat = repeat;
    cmd.stepSize = stepSize;
    this->queueCommand(cmd);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    group->toJSONRef(resp);
    resp.endObject();
    resp.endResponse();
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group with the specified id not found.\"}"));
}
void Web::handleTiltCommand(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(!this->isAuthenticated(request)) return;
  uint8_t shadeId = 255;
  uint8_t target = 255;
  somfy_commands command = somfy_commands::My;
  if(asyncHasParam(request, "shadeId")) {
    shadeId = asyncParam(request, "shadeId").toInt();
    if(asyncHasParam(request, "command")) command = translateSomfyCommand(asyncParam(request, "command"));
    else if(asyncHasParam(request, "target")) target = asyncParam(request, "target").toInt();
  }
  else if(!json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) shadeId = obj["shadeId"];
    else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade id was supplied.\"}")); return; }
    if(obj.containsKey("command")) { String scmd = obj["command"]; command = translateSomfyCommand(scmd); }
    else if(obj.containsKey("target")) target = obj["target"].as<uint8_t>();
  }
  else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade object supplied.\"}")); return; }
  SomfyShade *shade = somfy.getShadeById(shadeId);
  if(shade) {
    web_command_t cmd = {};
    cmd.type = web_cmd_t::tilt_command;
    cmd.shadeId = shadeId;
    cmd.target = target;
    cmd.command = command;
    this->queueCommand(cmd);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    shade->toJSONRef(resp);
    resp.endObject();
    resp.endResponse();
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade with the specified id not found.\"}"));
}
void Web::handleRepeatCommand(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(!this->isAuthenticated(request)) return;
  uint8_t shadeId = 255;
  uint8_t groupId = 255;
  uint8_t stepSize = 0;
  int8_t repeat = -1;
  somfy_commands command = somfy_commands::My;
  if(asyncHasParam(request, "shadeId")) shadeId = asyncParam(request, "shadeId").toInt();
  else if(asyncHasParam(request, "groupId")) groupId = asyncParam(request, "groupId").toInt();
  if(asyncHasParam(request, "command")) command = translateSomfyCommand(asyncParam(request, "command"));
  if(asyncHasParam(request, "repeat")) repeat = asyncParam(request, "repeat").toInt();
  if(asyncHasParam(request, "stepSize")) stepSize = asyncParam(request, "stepSize").toInt();
  if(shadeId == 255 && groupId == 255 && !json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) shadeId = obj["shadeId"];
    if(obj.containsKey("groupId")) groupId = obj["groupId"];
    if(obj.containsKey("stepSize")) stepSize = obj["stepSize"];
    if(obj.containsKey("command")) { String scmd = obj["command"]; command = translateSomfyCommand(scmd); }
    if(obj.containsKey("repeat")) repeat = obj["repeat"].as<uint8_t>();
  }
  if(shadeId != 255) {
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade reference could not be found.\"}")); return; }
    web_command_t cmd = {};
    cmd.type = web_cmd_t::shade_repeat;
    cmd.shadeId = shadeId;
    cmd.command = command;
    cmd.repeat = repeat;
    cmd.stepSize = stepSize;
    this->queueCommand(cmd);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    shade->toJSONRef(resp);
    resp.endObject();
    resp.endResponse();
  }
  else if(groupId != 255) {
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(!group) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group reference could not be found.\"}")); return; }
    web_command_t cmd = {};
    cmd.type = web_cmd_t::group_repeat;
    cmd.groupId = groupId;
    cmd.command = command;
    cmd.repeat = repeat;
    cmd.stepSize = stepSize;
    this->queueCommand(cmd);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    group->toJSONRef(resp);
    resp.endObject();
    resp.endResponse();
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Invalid Http method\"}"));
}
void Web::handleRoom(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(request->method() == HTTP_GET) {
    if(asyncHasParam(request, "roomId")) {
      int roomId = asyncParam(request, "roomId").toInt();
      SomfyRoom *room = somfy.getRoomById(roomId);
      if(room) {
        AsyncJsonResp resp;
        resp.beginResponse(request, g_content, sizeof(g_content));
        resp.beginObject();
        room->toJSON(resp);
        resp.endObject();
        resp.endResponse();
      }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Room Id not found.\"}"));
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"You must supply a valid room id.\"}"));
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Invalid Http method\"}"));
}
void Web::handleShade(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(request->method() == HTTP_GET) {
    if(asyncHasParam(request, "shadeId")) {
      int shadeId = asyncParam(request, "shadeId").toInt();
      SomfyShade *shade = somfy.getShadeById(shadeId);
      if(shade) {
        AsyncJsonResp resp;
        resp.beginResponse(request, g_content, sizeof(g_content));
        resp.beginObject();
        shade->toJSON(resp);
        resp.endObject();
        resp.endResponse();
      }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade Id not found.\"}"));
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"You must supply a valid shade id.\"}"));
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Invalid Http method\"}"));
}
void Web::handleGroup(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(request->method() == HTTP_GET) {
    if(asyncHasParam(request, "groupId")) {
      int groupId = asyncParam(request, "groupId").toInt();
      SomfyGroup *group = somfy.getGroupById(groupId);
      if(group) {
        AsyncJsonResp resp;
        resp.beginResponse(request, g_content, sizeof(g_content));
        resp.beginObject();
        group->toJSON(resp);
        resp.endObject();
        resp.endResponse();
      }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group Id not found.\"}"));
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"You must supply a valid group id.\"}"));
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Invalid Http method\"}"));
}
void Web::handleSetPositions(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  uint8_t shadeId = asyncHasParam(request, "shadeId") ? asyncParam(request, "shadeId").toInt() : 255;
  int8_t pos = asyncHasParam(request, "position") ? asyncParam(request, "position").toInt() : -1;
  int8_t tiltPos = asyncHasParam(request, "tiltPosition") ? asyncParam(request, "tiltPosition").toInt() : -1;
  if(!json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) shadeId = obj["shadeId"];
    if(obj.containsKey("position")) pos = obj["position"];
    if(obj.containsKey("tiltPosition")) tiltPos = obj["tiltPosition"];
  }
  if(shadeId != 255) {
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(shade) {
      web_command_t cmd = {};
      cmd.type = web_cmd_t::set_positions;
      cmd.shadeId = shadeId;
      cmd.position = pos;
      cmd.tiltPosition = tiltPos;
      this->queueCommand(cmd);
      AsyncJsonResp resp;
      resp.beginResponse(request, g_content, sizeof(g_content));
      resp.beginObject();
      shade->toJSON(resp);
      resp.endObject();
      resp.endResponse();
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"An invalid shadeId was provided\"}"));
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"shadeId was not provided\"}"));
}
void Web::handleSetSensor(AsyncWebServerRequest *request, JsonVariant &json) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  uint8_t shadeId = asyncHasParam(request, "shadeId") ? asyncParam(request, "shadeId").toInt() : 255;
  uint8_t groupId = asyncHasParam(request, "groupId") ? asyncParam(request, "groupId").toInt() : 255;
  int8_t sunny = asyncHasParam(request, "sunny") ? (toBoolean(asyncParam(request, "sunny").c_str(), false) ? 1 : 0) : -1;
  int8_t windy = asyncHasParam(request, "windy") ? asyncParam(request, "windy").toInt() : -1;
  int8_t repeat = asyncHasParam(request, "repeat") ? asyncParam(request, "repeat").toInt() : -1;
  if(!json.isNull()) {
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) shadeId = obj["shadeId"].as<uint8_t>();
    if(obj.containsKey("groupId")) groupId = obj["groupId"].as<uint8_t>();
    if(obj.containsKey("sunny")) {
      if(obj["sunny"].is<bool>()) sunny = obj["sunny"].as<bool>() ? 1 : 0;
      else sunny = obj["sunny"].as<int8_t>();
    }
    if(obj.containsKey("windy")) {
      if(obj["windy"].is<bool>()) windy = obj["windy"].as<bool>() ? 1 : 0;
      else windy = obj["windy"].as<int8_t>();
    }
    if(obj.containsKey("repeat")) repeat = obj["repeat"].as<uint8_t>();
  }
  if(shadeId != 255) {
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(shade) {
      web_command_t cmd = {};
      cmd.type = web_cmd_t::shade_sensor;
      cmd.shadeId = shadeId;
      cmd.sunny = sunny;
      cmd.windy = windy;
      cmd.repeat = repeat;
      this->queueCommand(cmd);
      AsyncJsonResp resp;
      resp.beginResponse(request, g_content, sizeof(g_content));
      resp.beginObject();
      shade->toJSON(resp);
      resp.endObject();
      resp.endResponse();
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"An invalid shadeId was provided\"}"));
  }
  else if(groupId != 255) {
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(group) {
      web_command_t cmd = {};
      cmd.type = web_cmd_t::group_sensor;
      cmd.groupId = groupId;
      cmd.sunny = sunny;
      cmd.windy = windy;
      cmd.repeat = repeat;
      this->queueCommand(cmd);
      AsyncJsonResp resp;
      resp.beginResponse(request, g_content, sizeof(g_content));
      resp.beginObject();
      group->toJSON(resp);
      resp.endObject();
      resp.endResponse();
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"An invalid groupId was provided\"}"));
  }
  else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"shadeId was not provided\"}"));
}
void Web::handleDownloadFirmware(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  GitRepo repo;
  GitRelease *rel = nullptr;
  int8_t err = repo.getReleases();
  if(err == 0) {
    if(asyncHasParam(request, "ver")) {
      String ver = asyncParam(request, "ver");
      if(ver == "latest") rel = &repo.releases[0];
      else if(ver == "main") rel = &repo.releases[GIT_MAX_RELEASES];
      else {
        for(uint8_t i = 0; i < GIT_MAX_RELEASES; i++) {
          if(repo.releases[i].id == 0) continue;
          if(strcmp(repo.releases[i].name, ver.c_str()) == 0) { rel = &repo.releases[i]; break; }
        }
      }
      if(rel) {
        AsyncJsonResp resp;
        resp.beginResponse(request, g_content, sizeof(g_content));
        resp.beginObject();
        rel->toJSON(resp);
        resp.endObject();
        resp.endResponse();
        strcpy(git.targetRelease, rel->name);
        git.status = GIT_AWAITING_UPDATE;
      }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Release not found in repo.\"}"));
    }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Release version not supplied.\"}"));
  }
  else request->send(err, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Error communicating with Github.\"}"));
}
void Web::handleBackup(AsyncWebServerRequest *request) {
  bool attach = false;
  if(asyncHasParam(request, "attach")) attach = toBoolean(asyncParam(request, "attach").c_str(), false);
  LOGI("Saving current shade information");
  somfy.writeBackup();
  File file = LittleFS.open("/controller.backup", "r");
  if(!file) {
    LOGE("Error opening controller.backup");
    request->send(500, _encoding_text, "Error opening backup file");
    return;
  }
  if(attach) {
    char filename[120];
    Timestamp ts;
    char *iso = ts.getISOTime();
    for(uint8_t i = 0; i < strlen(iso); i++) {
      if(iso[i] == '.') { iso[i] = '\0'; break; }
      if(iso[i] == ':') iso[i] = '_';
    }
    snprintf(filename, sizeof(filename), "attachment; filename=\"ESPSomfyRTS %s.backup\"", iso);
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/controller.backup", _encoding_text);
    response->addHeader("Content-Disposition", filename);
    response->addHeader("Access-Control-Expose-Headers", "Content-Disposition");
    request->send(response);
  } else {
    request->send(LittleFS, "/controller.backup", _encoding_text);
  }
  file.close();
}
void Web::handleReboot(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  if(request->method() == HTTP_POST || request->method() == HTTP_PUT) {
    LOGI("Rebooting ESP...");
    rebootDelay.reboot = true;
    rebootDelay.rebootTime = millis() + 500;
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully started reboot\"}");
  }
  else request->send(201, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"Invalid HTTP Method\"}");
}
void Web::handleNotFound(AsyncWebServerRequest *request) {
  if(request->method() == HTTP_OPTIONS) { request->send(200); return; }
  LOGW("Request %s 404", request->url().c_str());
  snprintf(g_content, sizeof(g_content), "404 Service Not Found: %s", request->url().c_str());
  request->send(404, _encoding_text, g_content);
}

// =====================================================
// Route Registration
// =====================================================
void Web::begin() {
  LOGI("Creating Web MicroServices...");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "PUT,POST,GET,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  // API Server (port 8081)
  asyncApiServer.on("/discovery", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleDiscovery(r); });
  asyncApiServer.on("/rooms", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetRooms(r); });
  asyncApiServer.on("/shades", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetShades(r); });
  asyncApiServer.on("/groups", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetGroups(r); });
  asyncApiServer.on("/controller", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleController(r); });
  asyncApiServer.on("/room", HTTP_GET, [](AsyncWebServerRequest *r) { webServer.handleRoom(r); });
  asyncApiServer.on("/shade", HTTP_GET, [](AsyncWebServerRequest *r) { webServer.handleShade(r); });
  asyncApiServer.on("/group", HTTP_GET, [](AsyncWebServerRequest *r) { webServer.handleGroup(r); });
  asyncApiServer.on("/downloadFirmware", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleDownloadFirmware(r); });
  asyncApiServer.on("/backup", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleBackup(r); });
  asyncApiServer.on("/reboot", HTTP_POST | HTTP_PUT, [](AsyncWebServerRequest *r) { webServer.handleReboot(r); });
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/shadeCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleShadeCommand(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/groupCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleGroupCommand(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/tiltCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleTiltCommand(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/repeatCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleRepeatCommand(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/setPositions", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleSetPositions(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/setSensor", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleSetSensor(r, j); }));
  asyncApiServer.addHandler(new AsyncCallbackJsonWebHandler("/login", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleLogin(r, j); }));
  asyncApiServer.on("/shadeCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleShadeCommand(r, v); });
  asyncApiServer.on("/groupCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleGroupCommand(r, v); });
  asyncApiServer.on("/tiltCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleTiltCommand(r, v); });
  asyncApiServer.on("/repeatCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleRepeatCommand(r, v); });
  asyncApiServer.on("/setPositions", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleSetPositions(r, v); });
  asyncApiServer.on("/setSensor", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleSetSensor(r, v); });
  asyncApiServer.on("/login", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleLogin(r, v); });
  asyncApiServer.onNotFound([](AsyncWebServerRequest *r) {
    if(r->method() == HTTP_OPTIONS) { r->send(200); return; }
    webServer.handleNotFound(r);
  });

  // Web Interface (port 80)
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/shadeCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleShadeCommand(r, j); }));
  asyncServer.on("/shadeCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleShadeCommand(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/groupCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleGroupCommand(r, j); }));
  asyncServer.on("/groupCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleGroupCommand(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/tiltCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleTiltCommand(r, j); }));
  asyncServer.on("/tiltCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleTiltCommand(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/repeatCommand", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleRepeatCommand(r, j); }));
  asyncServer.on("/repeatCommand", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleRepeatCommand(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setPositions", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleSetPositions(r, j); }));
  asyncServer.on("/setPositions", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleSetPositions(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setSensor", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleSetSensor(r, j); }));
  asyncServer.on("/setSensor", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleSetSensor(r, v); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/login", [](AsyncWebServerRequest *r, JsonVariant &j) { webServer.handleLogin(r, j); }));
  asyncServer.on("/login", HTTP_GET, [](AsyncWebServerRequest *r) { JsonVariant v; webServer.handleLogin(r, v); });
  asyncServer.on("/loginContext", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("type", static_cast<uint8_t>(settings.Security.type));
    resp.addElem("permissions", settings.Security.permissions);
    resp.addElem("serverId", settings.serverId);
    resp.addElem("version", settings.fwVersion.name);
    resp.addElem("model", "ESPSomfyRTS");
    resp.addElem("hostname", settings.hostname);
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/upnp.xml", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("text/xml");
    SSDP.schema(*response);
    request->send(response);
  });
  asyncServer.on("/shades.cfg", HTTP_GET, [](AsyncWebServerRequest *r) { r->send(LittleFS, "/shades.cfg", _encoding_text); });
  asyncServer.on("/shades.tmp", HTTP_GET, [](AsyncWebServerRequest *r) { r->send(LittleFS, "/shades.tmp", _encoding_text); });
  asyncServer.on("/getReleases", HTTP_GET, [](AsyncWebServerRequest *request) {
    GitRepo repo;
    repo.getReleases();
    git.setCurrentRelease(repo);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    repo.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/downloadFirmware", HTTP_GET | HTTP_PUT, [](AsyncWebServerRequest *r) { webServer.handleDownloadFirmware(r); });
  asyncServer.on("/cancelFirmware", HTTP_GET | HTTP_PUT, [](AsyncWebServerRequest *request) {
    if(!git.lockFS) {
      git.status = GIT_UPDATE_CANCELLING;
      AsyncJsonResp resp;
      resp.beginResponse(request, g_content, sizeof(g_content));
      resp.beginObject();
      git.toJSON(resp);
      resp.endObject();
      resp.endResponse();
      git.cancelled = true;
    } else {
      request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Cannot cancel during filesystem update.\"}"));
    }
  });
  asyncServer.on("/backup", HTTP_GET, [](AsyncWebServerRequest *r) { webServer.handleBackup(r); });
  asyncServer.on("/restore", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if(webServer.uploadSuccess) {
        request->send(200, _encoding_json, "{\"status\":\"Success\",\"desc\":\"Restoring Shade settings\"}");
        restore_options_t opts;
        if(asyncHasParam(request, "data")) {
          DynamicJsonDocument doc(256);
          DeserializationError err = deserializeJson(doc, asyncParam(request, "data"));
          if(!err) { JsonObject obj = doc.as<JsonObject>(); opts.fromJSON(obj); }
        } else { opts.shades = true; }
        ShadeConfigFile::restore(&somfy, "/shades.tmp", opts);
        LOGI("Rebooting ESP for restored settings...");
        rebootDelay.reboot = true;
        rebootDelay.rebootTime = millis() + 1000;
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      esp_task_wdt_reset();
      if(index == 0) {
        webServer.uploadSuccess = false;
        LOGI("Restore: %s", filename.c_str());
        File fup = LittleFS.open("/shades.tmp", "w");
        fup.close();
      }
      if(len > 0) {
        File fup = LittleFS.open("/shades.tmp", "a");
        fup.write(data, len);
        fup.close();
      }
      if(final) { webServer.uploadSuccess = true; }
    });
  asyncServer.on("/controller", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleController(r); });
  asyncServer.on("/rooms", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetRooms(r); });
  asyncServer.on("/shades", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetShades(r); });
  asyncServer.on("/groups", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGetGroups(r); });
  asyncServer.on("/room", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleRoom(r); });
  asyncServer.on("/shade", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleShade(r); });
  asyncServer.on("/group", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *r) { webServer.handleGroup(r); });
  asyncServer.on("/getNextRoom", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("roomId", somfy.getNextRoomId());
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/getNextShade", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t shadeId = somfy.getNextShadeId();
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("shadeId", shadeId);
    resp.addElem("remoteAddress", (uint32_t)somfy.getNextRemoteAddress(shadeId));
    resp.addElem("bitLength", somfy.transceiver.config.type);
    resp.addElem("stepSize", (uint8_t)100);
    resp.addElem("proto", static_cast<uint8_t>(somfy.transceiver.config.proto));
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/getNextGroup", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t groupId = somfy.getNextGroupId();
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("groupId", groupId);
    resp.addElem("remoteAddress", (uint32_t)somfy.getNextRemoteAddress(groupId));
    resp.addElem("bitLength", somfy.transceiver.config.type);
    resp.addElem("proto", static_cast<uint8_t>(somfy.transceiver.config.proto));
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/addRoom", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No room object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(somfy.roomCount() > SOMFY_MAX_ROOMS) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Maximum number of rooms exceeded.\"}")); return; }
    SomfyRoom *room = somfy.addRoom(obj);
    if(room) { AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); room->toJSON(resp); resp.endObject(); resp.endResponse(); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Error adding room.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/addShade", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(somfy.shadeCount() > SOMFY_MAX_SHADES) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Maximum number of shades exceeded.\"}")); return; }
    SomfyShade *shade = somfy.addShade(obj);
    if(shade) { AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse(); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Error adding shade.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/addGroup", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No group object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(somfy.groupCount() > SOMFY_MAX_GROUPS) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Maximum number of groups exceeded.\"}")); return; }
    SomfyGroup *group = somfy.addGroup(obj);
    if(group) { AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); group->toJSON(resp); resp.endObject(); resp.endResponse(); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Error adding group.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/saveRoom", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No room object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("roomId")) {
      SomfyRoom *room = somfy.getRoomById(obj["roomId"]);
      if(room) { room->fromJSON(obj); room->save(); AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); room->toJSON(resp); resp.endObject(); resp.endResponse(); }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Room Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No room id was supplied.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/saveShade", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) {
      SomfyShade *shade = somfy.getShadeById(obj["shadeId"]);
      if(shade) {
        int8_t err = shade->fromJSON(obj);
        if(err == 0) { shade->save(); AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse(); }
        else { snprintf(g_content, sizeof(g_content), "{\"status\":\"DATA\",\"desc\":\"Data Error.\", \"code\":%d}", err); request->send(500, _encoding_json, g_content); }
      } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade id was supplied.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/saveGroup", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No group object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("groupId")) {
      SomfyGroup *group = somfy.getGroupById(obj["groupId"]);
      if(group) { group->fromJSON(obj); group->save(); AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); group->toJSON(resp); resp.endObject(); resp.endResponse(); }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No group id was supplied.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setMyPosition", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255; int8_t pos = -1; int8_t tilt = -1;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; if(obj.containsKey("pos")) pos = obj["pos"].as<int8_t>(); if(obj.containsKey("tilt")) tilt = obj["tilt"].as<int8_t>(); }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(shade) { if(tilt < 0) tilt = shade->myPos; if(shade->tiltType == tilt_types::none) tilt = -1; if(pos >= 0 && pos <= 100) shade->setMyPosition(shade->transformPosition(pos), shade->transformPosition(tilt)); AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSONRef(resp); resp.endObject(); resp.endResponse(); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade with the specified id not found.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setRollingCode", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255; uint16_t rollingCode = 0;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; if(obj.containsKey("rollingCode")) rollingCode = obj["rollingCode"]; }
    SomfyShade *shade = (shadeId != 255) ? somfy.getShadeById(shadeId) : nullptr;
    if(!shade) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade not found to set rolling code\"}")); return; }
    shade->setRollingCode(rollingCode);
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setPaired", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255; bool paired = false;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; if(obj.containsKey("paired")) paired = obj["paired"]; }
    SomfyShade *shade = (shadeId != 255) ? somfy.getShadeById(shadeId) : nullptr;
    if(!shade) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade not found to pair\"}")); return; }
    shade->paired = paired; shade->save();
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/pairShade", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; }
    SomfyShade *shade = (shadeId != 255) ? somfy.getShadeById(shadeId) : nullptr;
    if(!shade) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade not found to pair\"}")); return; }
    if(shade->bitLength == 56) shade->sendCommand(somfy_commands::Prog, 7); else shade->sendCommand(somfy_commands::Prog, 1);
    shade->paired = true; shade->save();
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/unpairShade", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; }
    SomfyShade *shade = (shadeId != 255) ? somfy.getShadeById(shadeId) : nullptr;
    if(!shade) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade not found to unpair\"}")); return; }
    if(shade->bitLength == 56) shade->sendCommand(somfy_commands::Prog, 7); else shade->sendCommand(somfy_commands::Prog, 1);
    shade->paired = false; shade->save();
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/linkRemote", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No remote object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) {
      SomfyShade *shade = somfy.getShadeById(obj["shadeId"]);
      if(shade) { if(obj.containsKey("remoteAddress")) { if(obj.containsKey("rollingCode")) shade->linkRemote(obj["remoteAddress"], obj["rollingCode"]); else shade->linkRemote(obj["remoteAddress"]); } else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Remote address not provided.\"}")); return; } AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse(); }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade id was supplied.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/unlinkRemote", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No remote object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("shadeId")) {
      SomfyShade *shade = somfy.getShadeById(obj["shadeId"]);
      if(shade) { if(obj.containsKey("remoteAddress")) shade->unlinkRemote(obj["remoteAddress"]); else { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Remote address not provided.\"}")); return; } AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); shade->toJSON(resp); resp.endObject(); resp.endResponse(); }
      else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No shade id was supplied.\"}"));
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/linkToGroup", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No linking object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    uint8_t shadeId = obj.containsKey("shadeId") ? obj["shadeId"].as<uint8_t>() : 0;
    uint8_t groupId = obj.containsKey("groupId") ? obj["groupId"].as<uint8_t>() : 0;
    if(groupId == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group id not provided.\"}")); return; }
    if(shadeId == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade id not provided.\"}")); return; }
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(!group) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group id not found.\"}")); return; }
    group->linkShade(shadeId);
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); group->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/unlinkFromGroup", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No unlinking object supplied.\"}")); return; }
    JsonObject obj = json.as<JsonObject>();
    uint8_t shadeId = obj.containsKey("shadeId") ? obj["shadeId"].as<uint8_t>() : 0;
    uint8_t groupId = obj.containsKey("groupId") ? obj["groupId"].as<uint8_t>() : 0;
    if(groupId == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group id not provided.\"}")); return; }
    if(shadeId == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade id not provided.\"}")); return; }
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(!group) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group id not found.\"}")); return; }
    group->unlinkShade(shadeId);
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); group->toJSON(resp); resp.endObject(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/deleteRoom", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t roomId = 0;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("roomId")) roomId = obj["roomId"]; }
    SomfyRoom *room = somfy.getRoomById(roomId);
    if(!room) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Room with the specified id not found.\"}"));
    else { somfy.deleteRoom(roomId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Room deleted.\"}")); }
  }));
  asyncServer.on("/deleteRoom", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t roomId = asyncHasParam(request, "roomId") ? asyncParam(request, "roomId").toInt() : 0;
    SomfyRoom *room = somfy.getRoomById(roomId);
    if(!room) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Room with the specified id not found.\"}"));
    else { somfy.deleteRoom(roomId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Room deleted.\"}")); }
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/deleteShade", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t shadeId = 255;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("shadeId")) shadeId = obj["shadeId"]; }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade with the specified id not found.\"}"));
    else if(shade->isInGroup()) request->send(400, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"This shade is a member of a group and cannot be deleted.\"}"));
    else { somfy.deleteShade(shadeId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Shade deleted.\"}")); }
  }));
  asyncServer.on("/deleteShade", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t shadeId = asyncHasParam(request, "shadeId") ? asyncParam(request, "shadeId").toInt() : 255;
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Shade with the specified id not found.\"}"));
    else if(shade->isInGroup()) request->send(400, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"This shade is a member of a group and cannot be deleted.\"}"));
    else { somfy.deleteShade(shadeId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Shade deleted.\"}")); }
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/deleteGroup", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint8_t groupId = 255;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("groupId")) groupId = obj["groupId"]; }
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(!group) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group with the specified id not found.\"}"));
    else { somfy.deleteGroup(groupId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Group deleted.\"}")); }
  }));
  asyncServer.on("/deleteGroup", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t groupId = asyncHasParam(request, "groupId") ? asyncParam(request, "groupId").toInt() : 255;
    SomfyGroup *group = somfy.getGroupById(groupId);
    if(!group) request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group with the specified id not found.\"}"));
    else { somfy.deleteGroup(groupId); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Group deleted.\"}")); }
  });
  asyncServer.on("/updateFirmware", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if(Update.hasError()) request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"Error updating firmware\"}");
      else request->send(200, _encoding_json, "{\"status\":\"SUCCESS\",\"desc\":\"Successfully updated firmware\"}");
      rebootDelay.reboot = true; rebootDelay.rebootTime = millis() + 500;
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if(index == 0) { webServer.uploadSuccess = false; LOGI("Update: %s", filename.c_str()); if(!Update.begin(UPDATE_SIZE_UNKNOWN)) { LOGE("Update begin failed"); } else { somfy.transceiver.end(); mqtt.end(); } }
      if(len > 0) { if(Update.write(data, len) != len) { LOGE("Upload aborted"); Update.abort(); } }
      if(final) { if(Update.end(true)) { LOGI("Update Success: %u Rebooting...", index + len); webServer.uploadSuccess = true; } }
      esp_task_wdt_reset();
    });
  asyncServer.on("/updateApplication", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if(Update.hasError()) request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"Error updating application\"}");
      else request->send(200, _encoding_json, "{\"status\":\"SUCCESS\",\"desc\":\"Successfully updated application\"}");
      rebootDelay.reboot = true; rebootDelay.rebootTime = millis() + 500;
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if(index == 0) { webServer.uploadSuccess = false; LOGI("Update: %s", filename.c_str()); if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) { LOGE("Update begin failed"); } else { somfy.transceiver.end(); mqtt.end(); } }
      if(len > 0) { if(Update.write(data, len) != len) { LOGE("Upload aborted"); Update.abort(); } }
      if(final) { if(Update.end(true)) { webServer.uploadSuccess = true; LOGI("Update Success: %u Rebooting...", index + len); somfy.commit(); } else { somfy.commit(); } }
      esp_task_wdt_reset();
    });
  asyncServer.on("/scanaps", HTTP_GET, [](AsyncWebServerRequest *request) {
    esp_task_wdt_reset();
    esp_task_wdt_delete(NULL);
    if(net.softAPOpened) WiFi.disconnect(false);
    int n = WiFi.scanNetworks(false, true);
    esp_task_wdt_add(NULL);
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.beginObject("connected");
    resp.addElem("name", settings.WIFI.ssid);
    resp.addElem("passphrase", settings.WIFI.passphrase);
    resp.addElem("strength", (int32_t)WiFi.RSSI());
    resp.addElem("channel", (int32_t)WiFi.channel());
    resp.endObject();
    resp.beginArray("accessPoints");
    for(int i = 0; i < n; ++i) {
      if(WiFi.SSID(i).length() == 0 || WiFi.RSSI(i) < -95) continue;
      resp.beginObject();
      resp.addElem("name", WiFi.SSID(i).c_str());
      resp.addElem("channel", (int32_t)WiFi.channel(i));
      resp.addElem("strength", (int32_t)WiFi.RSSI(i));
      resp.addElem("macAddress", WiFi.BSSIDstr(i).c_str());
      resp.endObject();
    }
    resp.endArray();
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/reboot", HTTP_POST | HTTP_PUT, [](AsyncWebServerRequest *r) { webServer.handleReboot(r); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/saveSecurity", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(400, _encoding_html, "Error parsing JSON body"); return; }
    JsonObject obj = json.as<JsonObject>();
    settings.Security.fromJSON(obj);
    settings.Security.save();
    char token[65];
    webServer.createAPIToken(request->client()->remoteIP(), token);
    DynamicJsonDocument sdoc(1024);
    JsonObject sobj = sdoc.to<JsonObject>();
    settings.Security.toJSON(sobj);
    sobj["apiKey"] = token;
    serializeJson(sdoc, g_content, sizeof(g_content));
    request->send(200, _encoding_json, g_content);
  }));
  asyncServer.on("/getSecurity", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512);
    JsonObject obj = doc.to<JsonObject>();
    settings.Security.toJSON(obj);
    serializeJson(doc, g_content, sizeof(g_content));
    request->send(200, _encoding_json, g_content);
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/saveRadio", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(400, _encoding_html, "Error parsing JSON body"); return; }
    JsonObject obj = json.as<JsonObject>();
    somfy.transceiver.fromJSON(obj);
    somfy.transceiver.save();
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    somfy.transceiver.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  }));
  asyncServer.on("/getRadio", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    somfy.transceiver.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/sendRemoteCommand", [](AsyncWebServerRequest *request, JsonVariant &json) {
    somfy_frame_t frame; uint8_t repeats = 0;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); String scmd; if(obj.containsKey("address")) frame.remoteAddress = obj["address"]; if(obj.containsKey("command")) scmd = obj["command"].as<String>(); if(obj.containsKey("repeats")) repeats = obj["repeats"]; if(obj.containsKey("rcode")) frame.rollingCode = obj["rcode"]; if(obj.containsKey("encKey")) frame.encKey = obj["encKey"]; frame.cmd = translateSomfyCommand(scmd.c_str()); }
    if(frame.remoteAddress > 0 && frame.rollingCode > 0) { somfy.sendFrame(frame, repeats); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Command Sent\"}")); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No address or rolling code provided\"}"));
  }));
  asyncServer.on("/sendRemoteCommand", HTTP_GET, [](AsyncWebServerRequest *request) {
    somfy_frame_t frame; uint8_t repeats = 0;
    if(asyncHasParam(request, "address")) { frame.remoteAddress = atoi(asyncParam(request, "address").c_str()); if(asyncHasParam(request, "encKey")) frame.encKey = atoi(asyncParam(request, "encKey").c_str()); if(asyncHasParam(request, "command")) frame.cmd = translateSomfyCommand(asyncParam(request, "command")); if(asyncHasParam(request, "rcode")) frame.rollingCode = atoi(asyncParam(request, "rcode").c_str()); if(asyncHasParam(request, "repeats")) repeats = atoi(asyncParam(request, "repeats").c_str()); }
    if(frame.remoteAddress > 0 && frame.rollingCode > 0) { somfy.sendFrame(frame, repeats); request->send(200, _encoding_json, F("{\"status\":\"SUCCESS\",\"desc\":\"Command Sent\"}")); }
    else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No address or rolling code provided\"}"));
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setgeneral", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    if(obj.containsKey("hostname") || obj.containsKey("ssdpBroadcast") || obj.containsKey("checkForUpdate")) {
      bool checkForUpdate = settings.checkForUpdate;
      settings.fromJSON(obj); settings.save();
      if(settings.checkForUpdate != checkForUpdate) git.emitUpdateCheck();
      if(obj.containsKey("hostname")) net.updateHostname();
    }
    if(obj.containsKey("ntpServer")) { settings.NTP.fromJSON(obj); settings.NTP.save(); }
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set General Settings\"}");
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setNetwork", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(400, _encoding_html, "Error parsing JSON body"); return; }
    JsonObject obj = json.as<JsonObject>();
    bool reboot = false;
    if(obj.containsKey("connType") && obj["connType"].as<uint8_t>() != static_cast<uint8_t>(settings.connType)) { settings.connType = static_cast<conn_types_t>(obj["connType"].as<uint8_t>()); settings.save(); reboot = true; }
    if(obj.containsKey("wifi")) { JsonObject objWifi = obj["wifi"]; if(settings.connType == conn_types_t::wifi) { if(objWifi.containsKey("ssid") && objWifi["ssid"].as<String>().compareTo(settings.WIFI.ssid) != 0) { if(WiFi.softAPgetStationNum() == 0) reboot = true; } if(objWifi.containsKey("passphrase") && objWifi["passphrase"].as<String>().compareTo(settings.WIFI.passphrase) != 0) { if(WiFi.softAPgetStationNum() == 0) reboot = true; } } settings.WIFI.fromJSON(objWifi); settings.WIFI.save(); }
    if(obj.containsKey("ethernet")) { JsonObject objEth = obj["ethernet"]; if(settings.connType == conn_types_t::ethernet || settings.connType == conn_types_t::ethernetpref) reboot = true; settings.Ethernet.fromJSON(objEth); settings.Ethernet.save(); }
    if(reboot) { LOGI("Rebooting ESP for new Network settings..."); rebootDelay.reboot = true; rebootDelay.rebootTime = millis() + 1000; }
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set Network Settings\"}");
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/setIP", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    settings.IP.fromJSON(obj); settings.IP.save();
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set Network Settings\"}");
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/connectwifi", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    String ssid = "", passphrase = "";
    if(obj.containsKey("ssid")) ssid = obj["ssid"].as<String>();
    if(obj.containsKey("passphrase")) passphrase = obj["passphrase"].as<String>();
    bool reboot = false;
    if(ssid.compareTo(settings.WIFI.ssid) != 0) reboot = true;
    if(passphrase.compareTo(settings.WIFI.passphrase) != 0) reboot = true;
    if(!settings.WIFI.ssidExists(ssid.c_str()) && ssid.length() > 0) { request->send(400, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"WiFi Network Does not exist\"}"); return; }
    SETCHARPROP(settings.WIFI.ssid, ssid.c_str(), sizeof(settings.WIFI.ssid));
    SETCHARPROP(settings.WIFI.passphrase, passphrase.c_str(), sizeof(settings.WIFI.passphrase));
    settings.WIFI.save(); settings.WIFI.print();
    request->send(201, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set server connection\"}");
    if(reboot) { LOGI("Rebooting ESP for new WiFi settings..."); rebootDelay.reboot = true; rebootDelay.rebootTime = millis() + 1000; }
  }));
  asyncServer.on("/modulesettings", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    resp.addElem("fwVersion", settings.fwVersion.name);
    settings.toJSON(resp);
    settings.NTP.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.on("/networksettings", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    settings.toJSON(resp);
    resp.addElem("fwVersion", settings.fwVersion.name);
    resp.beginObject("ethernet");
    settings.Ethernet.toJSON(resp);
    resp.endObject();
    resp.beginObject("wifi");
    settings.WIFI.toJSON(resp);
    resp.endObject();
    resp.beginObject("ip");
    settings.IP.toJSON(resp);
    resp.endObject();
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/connectmqtt", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    mqtt.disconnect();
    settings.MQTT.fromJSON(obj); settings.MQTT.save();
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    settings.MQTT.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  }));
  asyncServer.on("/mqttsettings", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncJsonResp resp;
    resp.beginResponse(request, g_content, sizeof(g_content));
    resp.beginObject();
    settings.MQTT.toJSON(resp);
    resp.endObject();
    resp.endResponse();
  });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/roomSortOrder", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonArray arr = json.as<JsonArray>(); uint8_t order = 0;
    for(JsonVariant v : arr) { uint8_t roomId = v.as<uint8_t>(); if(roomId != 0) { SomfyRoom *room = somfy.getRoomById(roomId); if(room) room->sortOrder = order++; } }
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set room order\"}");
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/shadeSortOrder", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonArray arr = json.as<JsonArray>(); uint8_t order = 0;
    for(JsonVariant v : arr) { uint8_t shadeId = v.as<uint8_t>(); if(shadeId != 255) { SomfyShade *shade = somfy.getShadeById(shadeId); if(shade) shade->sortOrder = order++; } }
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set shade order\"}");
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/groupSortOrder", [](AsyncWebServerRequest *request, JsonVariant &json) {
    if(json.isNull()) { request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"JSON parse error\"}"); return; }
    JsonArray arr = json.as<JsonArray>(); uint8_t order = 0;
    for(JsonVariant v : arr) { uint8_t groupId = v.as<uint8_t>(); if(groupId != 255) { SomfyGroup *group = somfy.getGroupById(groupId); if(group) group->sortOrder = order++; } }
    request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Successfully set group order\"}");
  }));
  asyncServer.on("/beginFrequencyScan", HTTP_GET | HTTP_PUT, [](AsyncWebServerRequest *request) {
    somfy.transceiver.beginFrequencyScan();
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); somfy.transceiver.toJSON(resp); resp.endObject(); resp.endResponse();
  });
  asyncServer.on("/endFrequencyScan", HTTP_GET | HTTP_PUT, [](AsyncWebServerRequest *request) {
    somfy.transceiver.endFrequencyScan();
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginObject(); somfy.transceiver.toJSON(resp); resp.endObject(); resp.endResponse();
  });
  asyncServer.on("/recoverFilesystem", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request) {
    if(git.status == GIT_UPDATING) request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Filesystem is updating.  Please wait!!!\"}");
    else if(git.status != GIT_STATUS_READY) request->send(200, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"Cannot recover file system at this time.\"}");
    else { git.recoverFilesystem(); request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Recovering filesystem from github please wait!!!\"}"); }
  });
  asyncServer.on("/init/shades", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!webServer.isAuthenticated(request, true)) return;
    ShadeConfigFile nf;
    if(nf.begin("/shades.cfg", false)) {
      nf.header.version = 24; nf.header.length = 76;
      nf.header.roomRecordSize = 29; nf.header.roomRecords = 0;
      nf.header.shadeRecordSize = 276; nf.header.shadeRecords = 0;
      nf.header.groupRecordSize = 200; nf.header.groupRecords = 0;
      nf.header.repeaterRecordSize = 77; nf.header.repeaterRecords = 0;
      nf.header.settingsRecordSize = 0; nf.header.netRecordSize = 0; nf.header.transRecordSize = 0;
      nf.writeHeader(); nf.end();
      request->send(200, _encoding_json, "{\"status\":\"OK\",\"desc\":\"Created shades.cfg\"}");
    } else request->send(500, _encoding_json, "{\"status\":\"ERROR\",\"desc\":\"Failed to create shades.cfg\"}");
  });
  asyncServer.on("/groupOptions", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(asyncHasParam(request, "groupId")) {
      int groupId = asyncParam(request, "groupId").toInt();
      SomfyGroup *group = somfy.getGroupById(groupId);
      if(group) {
        AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content));
        resp.beginObject(); group->toJSON(resp);
        resp.beginArray("availShades");
        for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
          SomfyShade *shade = &somfy.shades[i];
          if(shade->getShadeId() != 255) {
            bool isLinked = false;
            for(uint8_t j = 0; j < SOMFY_MAX_GROUPED_SHADES; j++) { if(group->linkedShades[j] == shade->getShadeId()) { isLinked = true; break; } }
            if(!isLinked) { resp.beginObject(); shade->toJSONRef(resp); resp.endObject(); }
          }
        }
        resp.endArray(); resp.endObject(); resp.endResponse();
      } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"Group Id not found.\"}"));
    } else request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"You must supply a valid group id.\"}"));
  });
  asyncServer.on("/repeaters", HTTP_GET, [](AsyncWebServerRequest *r) { webServer.handleGetRepeaters(r); });
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/linkRepeater", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint32_t address = 0;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("address")) address = obj["address"]; else if(obj.containsKey("remoteAddress")) address = obj["remoteAddress"]; }
    if(address == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No repeater address was supplied.\"}")); return; }
    somfy.linkRepeater(address);
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginArray(); somfy.toJSONRepeaters(resp); resp.endArray(); resp.endResponse();
  }));
  asyncServer.addHandler(new AsyncCallbackJsonWebHandler("/unlinkRepeater", [](AsyncWebServerRequest *request, JsonVariant &json) {
    uint32_t address = 0;
    if(!json.isNull()) { JsonObject obj = json.as<JsonObject>(); if(obj.containsKey("address")) address = obj["address"]; else if(obj.containsKey("remoteAddress")) address = obj["remoteAddress"]; }
    if(address == 0) { request->send(500, _encoding_json, F("{\"status\":\"ERROR\",\"desc\":\"No repeater address was supplied.\"}")); return; }
    somfy.unlinkRepeater(address);
    AsyncJsonResp resp; resp.beginResponse(request, g_content, sizeof(g_content)); resp.beginArray(); somfy.toJSONRepeaters(resp); resp.endArray(); resp.endResponse();
  }));

  asyncServer.onNotFound([](AsyncWebServerRequest *r) {
    if(r->method() == HTTP_OPTIONS) { r->send(200); return; }
    webServer.handleNotFound(r);
  });

  // serveStatic MUST be registered AFTER all route handlers
  asyncServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  asyncServer.begin();
}
