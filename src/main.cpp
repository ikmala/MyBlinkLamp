#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "Wire.h"
#include "PCF8575.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Preferences.h>

#define AP_SSID "VOC_"
#define AP_PASSWORD "gvkJ1dFS"
const char* CONFIG_FILE = "/relay_config.json";

#define MODE_SWITCH 5
#define TRANSISTOR_PIN 4
#define RELAY_CONTROL 15
#define LED_WIFI 2
#define BUZZER 19

PCF8575 pcf1(0x21);
PCF8575 pcf2(0x22);
// PCF8575 pcf3(0x23);

uint8_t numRelays = 32;
bool modeOtomatis;

bool* relayState;
String receivedData = "";
bool wifiConnected = false;
bool lastModeState = HIGH;
unsigned long lastWiFiCheck = 0;
unsigned long lastBuzzerTime = 0;
bool buzzerActive = false;

// Pusher Cunfiguration
const char* pusher_app_key = "1b27884f6f602f5ef9ac";
const char* pusher_cluster = "ap1";

// Channels
const char* pusher_channel = "iot.channel";
const char* pusher_channel_add = "iot.add.channel";
const char* pusher_channel_dell = "iot.dell.channel";
const char* pusher_channel_edit = "iot.edit.channel";
const char* pusher_channel_warn = "iot.warn.channel";
const char* pusher_channel_check = "iot.check.channel";
const char* pusher_channel_test = "iot.test.channel";

// Events
const char* pusher_event_connec = "lamp.connection";
const char* pusher_event_add = "lamp.add";
const char* pusher_event_dell = "lamp.dell";
const char* pusher_event_edit = "lamp.edit";
const char* pusher_event_warn = "lamp.warn";
const char* pusher_event_check = "lamp.check";
const char* pusher_event_test = "lamp.test";

WebSocketsClient webSocket;
AsyncWebServer server(80);
Preferences preferences;

// Forward declarations
void controlManual();
void LoadRelayConfigSPIFFS();
void PrintRelayConfigSPIFFS();
void setRelayState(uint8_t channel, bool state);
void attemptWiFiConnection();
void ConnectWebSocket();
void startAPMode();
void cekWiFi(unsigned long currentMillis);
void buzzerBeep(int duration);
void PowerRelayByWebSocket(int code, const char* status);
void PowerRelayByWebSocketCheck(uint8_t code);
void PowerRelayByWebSocketWarning(uint8_t code);
void PowerRelayByWebSocketTest();
void clearWiFiConfig();

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(MODE_SWITCH, INPUT_PULLUP);
  pinMode(RELAY_CONTROL, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(TRANSISTOR_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_PIN, LOW);
  digitalWrite(RELAY_CONTROL, LOW);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(BUZZER, LOW);

  pcf1.begin();
  pcf2.begin();
  // pcf3.begin();

  for (int i = 0; i < 16; i++) {
    pcf1.pinMode(i, OUTPUT);
    pcf2.pinMode(i, OUTPUT);
    // pcf3.pinMode(i, OUTPUT);
    pcf1.digitalWrite(i, LOW);
    pcf2.digitalWrite(i, LOW);
    // pcf3.digitalWrite(i, LOW);
  }

  lastModeState = digitalRead(MODE_SWITCH);

  if (lastModeState == LOW) {
    controlManual();
    Serial.println("Boot: MODE MANUAL (relay langsung dimatikan sekali)");
  } else {
    Serial.println("Boot: MODE OTOMATIS (gunakan konfigurasi SPIFFS)");
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("Gagal memulai SPIFFS");
    // return;
  } else {
    Serial.println("SPIFFS Sap Digunakan");
  }

  relayState = new bool[numRelays];
  for (int i = 0; i < numRelays; i++) relayState[i] = false;

  LoadRelayConfigSPIFFS();
  PrintRelayConfigSPIFFS();
  for (int i = 0; i < numRelays; i++) {
    setRelayState(i, relayState[i]);
  }
  // SaveRelayConfigSPIFFS();
  preferences.begin("wifi-config", false);
  // clearWiFiConfig();     //buka komen ini jika ingin mengkosongkan penyimpanan konfigurasi wifi
  attemptWiFiConnection();

  if (wifiConnected) {
    ConnectWebSocket();
  } else {
    startAPMode();
  }
  // connectToWiFi();

  // Setup pcf Lampu
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
}

void loop() {
  unsigned long currentMillis = millis();

  cekWiFi(currentMillis);
  // bacaSerialBilling();
  if (wifiConnected) {
    webSocket.loop();
  }

  modeOtomatis = digitalRead(MODE_SWITCH);

  // if (!modeOtomatis) {
  //   controlManual();
  //   Serial.println("!modeOtomatis");
  //   delay(600);
  // }

  if (modeOtomatis != lastModeState) {
    lastModeState = modeOtomatis;
    if (modeOtomatis) {
      LoadRelayConfigSPIFFS();
      PrintRelayConfigSPIFFS();
      for (int i = 0; i < numRelays; i++) {
        setRelayState(i, relayState[i]);
      }
    } else {
      controlManual();
      Serial.println("MODE MANUAL: Semua relay dimatikan");
    }
    buzzerBeep(500);
    Serial.printf("Ubah Last Mode");
    Serial.println(modeOtomatis);
    delay(600);
  }

  if (buzzerActive && currentMillis - lastBuzzerTime >= 500) {
    digitalWrite(BUZZER, LOW);
    buzzerActive = false;
  }
}

void clearWiFiConfig() {
  preferences.clear();
  Serial.println("Konfigurasi WiFi telah dihapus. ESP32 akan masuk ke mode AP saat di-restart.");
}

void attemptWiFiConnection() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  String mode = preferences.getString("mode", "");
  String ip = preferences.getString("ip", "");
  String gateway = preferences.getString("gateway", "");
  String subnet = preferences.getString("subnet", "");

  if (ssid == "" || password == "") {
    Serial.println("Tidak ada konfigurasi WiFi yang tersimpan. Masuk ke mode AP.");
    return;
  }

  Serial.print("Mencoba menghubungkan ke WiFi: ");
  Serial.println(ssid);

  if (mode == "static") {
    IPAddress localIP, gatewayIP, subnetMask, dns1, dns2;
    localIP.fromString(ip);
    gatewayIP.fromString(gateway);
    subnetMask.fromString(subnet);
    dns1.fromString("8.8.8.8");
    dns2.fromString("8.8.4.4");
    WiFi.config(localIP, gatewayIP, subnetMask, dns1, dns2);
  }

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    digitalWrite(LED_WIFI, HIGH);
    Serial.println("\nTerhubung ke WiFi!");
    Serial.println(WiFi.localIP());
    buzzerBeep(1000);
    digitalWrite(TRANSISTOR_PIN, HIGH);
  } else {
    Serial.println("\nGagal terhubung ke WiFi yang tersimpan.");
    WiFi.disconnect();
  }
}

void startAPMode() {
  Serial.println("Memulai mode Access Point...");
  WiFi.mode(WIFI_AP);
  uint64_t mac = ESP.getEfuseMac();
  String apName = AP_SSID + String((uint16_t)(mac >> 32), HEX);
  WiFi.softAP(apName, AP_PASSWORD);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    int n = WiFi.scanNetworks();
    DynamicJsonDocument doc(1024);
    JsonArray ssids = doc.createNestedArray("ssids");
    for (int i = 0; i < n; i++) {
      ssids.add(WiFi.SSID(i));
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){
    String ssid = request->arg("ssid");
    String password = request->arg("password");
    String mode = request->arg("mode");
    String ip = request->arg("ip");
    String gateway = request->arg("gateway");
    String subnet = request->arg("subnet");

    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("mode", mode);
    if (mode == "static") {
      preferences.putString("ip", ip);
      preferences.putString("gateway", gateway);
      preferences.putString("subnet", subnet);
    }

    DynamicJsonDocument doc(256);
    doc["status"] = "ok";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);

    delay(1000);
    ESP.restart();
  });

  server.begin();
  Serial.println("Server AP dimulai di 192.168.4.1");
}

void cekWiFi(unsigned long currentMillis) {
  if (currentMillis - lastWiFiCheck >= 5000) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnected) {
        Serial.println("WiFi terputus!");
        // digitalWrite(LED_WIFI, LOW);
        wifiConnected = false;
        buzzerBeep(1000);
      }
    }
  }
}

void WebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("Disconnected");
      digitalWrite(16, LOW);
      digitalWrite(LED_WIFI, LOW);
      break;
    case WStype_CONNECTED:
      {
        Serial.println("Connected");
        digitalWrite(LED_WIFI, HIGH);
        String authMessage = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel) + "\"}}";
        webSocket.sendTXT(authMessage);
        String authMessageAdd = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_add) + "\"}}";
        webSocket.sendTXT(authMessageAdd);
        String authMessageDell = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_dell) + "\"}}";
        webSocket.sendTXT(authMessageDell);
        String authMessageEdit = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_edit) + "\"}}";
        webSocket.sendTXT(authMessageEdit);
        String authMessageCheck = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_check) + "\"}}";
        webSocket.sendTXT(authMessageCheck);
        String authMessageWarn = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_warn) + "\"}}";
        webSocket.sendTXT(authMessageWarn);
        String authMessageTest = "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + String(pusher_channel_test) + "\"}}";
        webSocket.sendTXT(authMessageTest);

        digitalWrite(16, HIGH);

        break;
      }
    case WStype_TEXT:
      {
        Serial.printf("Received text: %s\n", payload);

        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);

        const char* event = doc["event"];

        int eventCode = 0;
        if (strcmp(event, pusher_event_connec) == 0) {
          eventCode = 1;
        } else if (strcmp(event, pusher_event_add) == 0) {
          eventCode = 2;
        } else if (strcmp(event, pusher_event_dell) == 0) {
          eventCode = 3;
        } else if (strcmp(event, pusher_event_edit) == 0) {
          eventCode = 4;
        } else if (strcmp(event, pusher_event_check) == 0) {
          eventCode = 5;
        } else if (strcmp(event, pusher_event_warn) == 0) {
          eventCode = 6;
        } else if (strcmp(event, pusher_event_test) == 0) {
          eventCode = 7;
        }

        switch (eventCode) {
          case 1:  // Lamp ON/OFF
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              uint8_t code = dataDoc["code"];
              const char* status = dataDoc["status"];

              PowerRelayByWebSocket(code, status);
              break;
            }
          case 2:  // Add Lamp and Button Pin
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              int code = dataDoc["code"];

              // AddLampButtonPin(code);
              PrintRelayConfigSPIFFS();
              break;
            }
          case 3:  // Delete Lamp
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              int code = dataDoc["code"];

              // DellLampPin(code);
              PrintRelayConfigSPIFFS();
              break;
            }
          case 4:  // Edit Lamp and Button Pin
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              int code = dataDoc["code"];
              int pin_relay = dataDoc["pin_relay"];
              int pin_button = dataDoc["pin_button"];

              // EditLampButtonPin(code, pin_relay, pin_button);
              PrintRelayConfigSPIFFS();
              break;
            }
          case 5:
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              uint8_t code = dataDoc["code"];
              PowerRelayByWebSocketCheck(code);
              break;
            }
          case 6:
            {
              const char* dataPayload = doc["data"];
              DynamicJsonDocument dataDoc(1024);
              deserializeJson(dataDoc, dataPayload);

              uint8_t code = dataDoc["code"];
              PowerRelayByWebSocketWarning(code);
              break;
            }
          case 7:
            {
              PowerRelayByWebSocketTest();
              break;
            }
          default:
            Serial.println("Unknown event");
            break;
        }
        break;
      }
  }
}

void ConnectWebSocket() {
  String host = String("ws-") + pusher_cluster + ".pusher.com";
  String url = String("/app/") + pusher_app_key + "?protocol=7&client=esp32&version=1.0&flash=false";
  webSocket.beginSSL(host.c_str(), 443, url.c_str(), "", "wss");

  webSocket.onEvent(WebSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void SaveRelayConfigSPIFFS() {
  DynamicJsonDocument doc(1024);
  doc["numRelays"] = numRelays;

  JsonArray relaysArray = doc.createNestedArray("relayState");
  for (int i = 0; i < numRelays; i++) {
    JsonObject relayObj = relaysArray.createNestedObject();
    relayObj["state"] = relayState[i];
  }

  File configFile = SPIFFS.open(CONFIG_FILE, FILE_WRITE);
  if (!configFile) {
    Serial.println("Erorr: Gagal Membuka File!!!");
    return;
  }

  serializeJson(doc, configFile);
  configFile.close();
}

void LoadRelayConfigSPIFFS() {
  File configFile = SPIFFS.open(CONFIG_FILE, FILE_READ);
  if (!configFile) {
    Serial.println("Error: File Tidak Ditemukan!!!");
    return;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Error: Config File Terlalu Besar");
    return;
  }

  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, buf.get());
  if (error) {
    Serial.println("Error: Gagal Melakukan Pars JSON di File");
    return;
  }

  numRelays = doc["numRelays"];
  JsonArray relaysStateArray = doc["relayState"];

  delete[] relayState;
  relayState = new bool[numRelays];

  for (uint8_t i = 0; i < numRelays; i++) {
    relayState[i] = relaysStateArray[i]["state"];
  }

  configFile.close();
}

void PrintRelayConfigSPIFFS() {
  if (!SPIFFS.exists(CONFIG_FILE)) {
    Serial.println("File Tidak Ada");
    return;
  }

  File configFile = SPIFFS.open(CONFIG_FILE, FILE_READ);
  if (!configFile) {
    Serial.println("Error: Gagal Membuka File Config");
    return;
  }

  Serial.println("Config file content:");
  while (configFile.available()) {
    Serial.write(configFile.read());
  }

  configFile.close();
}

// 🔹 Fungsi ini mematikan semua relay di PCF8575 saat mode manual aktif
void controlManual() {
  for (uint8_t i = 0; i < 16; i++) {
    pcf1.digitalWrite(i, LOW);
    pcf2.digitalWrite(i, LOW);
    // pcf3.digitalWrite(i, LOW);
  }
  Serial.println("Mode Manual Aktif: Modul PCF Stanby");
  digitalWrite(RELAY_CONTROL, LOW);
}

void bacaSerialBilling() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      receivedData.trim();

      bool modeOtomatis = digitalRead(MODE_SWITCH);

      if (!modeOtomatis) {
        Serial.println("Mode Manual Aktif: Perintah Diabaikan");
        receivedData = "";
        return;
      }

      if (receivedData.startsWith("ON:")) {
        if (receivedData.substring(3) == "ALL") {
          for (uint8_t i = 0; i < 48; i++) setRelayState(i, true);
          Serial.println("Semua Channel ON");
        } else {
          uint8_t ch = receivedData.substring(3).toInt();
          if (ch >= 0 && ch < 48) {
            setRelayState(ch, true);
            Serial.print("Channel ");
            Serial.print(ch);
            Serial.println(" ON");
          }
        }
        buzzerBeep(500);
      } else if (receivedData.startsWith("OFF:")) {
        if (receivedData.substring(4) == "ALL") {
          for (uint8_t i = 0; i < 48; i++) setRelayState(i, false);
          Serial.println("Semua Channel OFF");
        } else {
          uint8_t ch = receivedData.substring(4).toInt();
          if (ch >= 0 && ch < 48) {
            setRelayState(ch, false);
            Serial.print("Channel ");
            Serial.print(ch);
            Serial.println(" OFF");
          }
        }
        buzzerBeep(500);
      } else if (receivedData == "WIFI?") {
        Serial.println(wifiConnected ? "WiFi Terhubung" : "WiFi Tidak Terhubung");
        if (wifiConnected) Serial.println(WiFi.localIP());
      }
      receivedData = "";
    } else {
      receivedData += c;
    }
  }
}

void setRelayState(uint8_t channel, bool state) {
  relayState[channel] = state;

  if (channel < 16) {
    pcf1.digitalWrite(channel, state ? HIGH : LOW);
  } else if (channel < 32) {
    pcf2.digitalWrite(channel - 16, state ? HIGH : LOW);
  }
  //  else {
  // pcf3.digitalWrite(channel - 32, state ? HIGH : LOW);
  // }
}

void buzzerBeep(int duration) {
  digitalWrite(BUZZER, HIGH);
  delay(duration);
  digitalWrite(BUZZER, LOW);
}

void PowerRelayByWebSocket(int code, const char* status) {
  Serial.print("Code: ");
  Serial.println(code);
  Serial.print("Status: ");
  Serial.println(status);

  // bool isFound = false;
  // for (int i = 0; i < numRelays; i++) {
  //   if (relays[i].code == code - 1) isFound = true;
  // }

  if (code > numRelays) {
    Serial.println("Error: Invalid code");
    return;
  }

  bool activateRelay = strcmp(status, "active") == 0;

  if (activateRelay) {
    setRelayState(code - 1, true);
    buzzerBeep(500);
  } else {
    setRelayState(code - 1, false);
    buzzerBeep(500);
  }

  Serial.print("Relay ");
  Serial.print(code);
  Serial.println(activateRelay ? " ON" : " OFF");

  SaveRelayConfigSPIFFS();
  PrintRelayConfigSPIFFS();
}

void PowerRelayByWebSocketWarning(uint8_t code) {
  Serial.print("Warned Code: ");
  Serial.print(code);

  setRelayState(code - 1, false);
  buzzerBeep(400);
  setRelayState(code - 1, true);
  buzzerBeep(400);
  setRelayState(code - 1, false);
  buzzerBeep(400);
  setRelayState(code - 1, true);
  buzzerBeep(400);
}

void PowerRelayByWebSocketCheck(uint8_t code) {
  Serial.print("Checked Code: ");
  Serial.print(code);

  int previousStates = relayState[code - 1];

  if (previousStates) {
    setRelayState(code - 1, false);
    buzzerBeep(500);
    SaveRelayConfigSPIFFS();
  } else {
    setRelayState(code - 1, true);
    buzzerBeep(500);
    SaveRelayConfigSPIFFS();
  }
}

void PowerRelayByWebSocketTest() {
  Serial.print("Warned Code: ");

  bool previousStates[numRelays];

  for (int i = 0; i < numRelays; i++) {
    previousStates[i] = relayState[i];
  }

  // Matikan Semua Lampu Terlebih Dahulu
  for (uint8_t i = 0; i < numRelays; i++) {
    if (relayState[i]) {
      setRelayState(i, false);
      // buzzerBeep(500);
    }
  }

  // Nyalakan Semua
  for (uint8_t i = 0; i < numRelays; i++) {
    setRelayState(i, true);
    buzzerBeep(500);
    delay(200);
  }

  // Matikan Semua
  for (uint8_t i = 0; i < numRelays; i++) {
    setRelayState(i, false);
    buzzerBeep(500);
    delay(200);
  }

  // Kembalikan Lampu Yang Awalnya Sudah Hidup
  for (uint8_t i = 0; i < numRelays; i++) {
    setRelayState(i, previousStates[i]);
    // buzzerBeep(500);
  }

  Serial.print("Testing Done....");
}
