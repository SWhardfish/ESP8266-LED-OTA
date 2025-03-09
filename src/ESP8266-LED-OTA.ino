#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <WiFiClientSecureBearSSL.h>

#define LED_PIN D6         // The ESP8266 pin connected to LED
#define SWITCH_PIN D5      // The ESP8266 pin connected to the momentary switch
#define STATUS_LED D4      // Status LED for WiFi connection feedback

String current_version = VERSION;
const String api_url = "https://api.github.com/repos/SWhardfish/ESP8266-LED-OTA/releases/latest"; // GitHub API for latest release
const char *firmware_url = "https://github.com/SWhardfish/ESP8266-LED-OTA/releases/latest/download/firmware.bin"; // URL to firmware binary

String ssid = "";
String password = "";

ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // Sync every 60 seconds

int LED_state = LOW;
int switchState = HIGH;
int lastSwitchState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// Reboot time variables
const int rebootHour = 3; // Default reboot time at 03:00
const int rebootMinute = 0;
bool rebootTriggered = false;

// LED schedule variables
const int morningOnHour = 6;
const int morningOnMinute = 30;
const int morningOffHour = 9;
const int morningOffMinute = 0;
const int eveningOffHour = 23;
const int eveningOffMinute = 30;

// Status LED variables
unsigned long previousMillis = 0;
const long wifiBlinkInterval = 500;  // Blink interval for WiFi connection (500ms)
const long apBlinkInterval = 1000;  // Blink interval for AP mode (1000ms)
bool statusLedState = LOW;

int getSunsetHour() {
    return 17; // 5 PM
}

void loadConfig() {
    if (!LittleFS.begin()) {
        Serial.println("Failed to mount LittleFS");
        return;
    }

    File configFile = LittleFS.open("/config.json", "r");
    if (!configFile) {
        Serial.println("Failed to open config file");
        return;
    }

    size_t size = configFile.size();
    if (size > 1024) {
        Serial.println("Config file too large");
        return;
    }

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, configFile);
    if (error) {
        Serial.println("Failed to parse config file");
        return;
    }

    ssid = doc["ssid"].as<String>();
    password = doc["password"].as<String>();

    Serial.println("WiFi Config Loaded:");
    Serial.println("SSID: " + ssid);

    configFile.close();
}

void saveConfig() {
    DynamicJsonDocument doc(256);
    doc["ssid"] = ssid;
    doc["password"] = password;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
        Serial.println("Failed to open config file for writing");
        return;
    }

    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("WiFi Config Saved");
}

void startAPMode() {
    WiFi.softAP("ESP8266-Config");  // Start AP with SSID "ESP8266-Config"
    Serial.println("Access Point Started");
    Serial.println("IP Address: " + WiFi.softAPIP().toString());

    // Serve the configuration page
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", getHTML());
    });

    // Handle form submission for /setwifi
    server.on("/setwifi", HTTP_POST, []() {
        ssid = server.arg("ssid");
        password = server.arg("password");
        saveConfig();
        server.send(200, "text/plain", "WiFi credentials saved. Rebooting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();  // Start the web server

    // Give some time to establish AP mode connection
    unsigned long startTime = millis();
    while (millis() - startTime < 120000) {  // Allow AP mode for 120 seconds
        server.handleClient();  // Process requests
        delay(100);  // Delay to allow for AP mode to be active
    }

    // After timeout, attempt to reconnect to WiFi
    Serial.println("Timeout reached in AP mode, retrying WiFi connection...");
    connectWiFi();  // Try connecting to WiFi again
}

void connectWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print("...");
        digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); // Blink while connecting
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected to WiFi!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        digitalWrite(STATUS_LED, LOW); // WiFi connected, turn LED off
        // Start server here, only after successful WiFi connection
        setupRoutes();
        server.begin();  // Start the web server
    } else {
        Serial.println("\nFailed to connect. Starting AP mode...");
        startAPMode(); // If WiFi connection fails, start AP mode
    }
}

void startOTA() {
    ArduinoOTA.onStart([]() {
        Serial.println("OTA Update Started...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("OTA Update Complete. Rebooting...");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("OTA Ready.");
}

void removePreviousOTAFile() {
    if (LittleFS.exists("/firmware.bin")) {
        LittleFS.remove("/firmware.bin");
        Serial.println("Previous OTA file removed.");
    }
}

String updateStatus = "";
String rebootStatus = "";

void checkForUpdates() {
    Serial.println("Checking for firmware updates...");
    updateStatus = "Checking for firmware updates...";

    removePreviousOTAFile();

    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
    HTTPClient http;

    client->setInsecure();  // Ignore SSL validation
    http.begin(*client, api_url);
    http.setTimeout(20000);

    int httpCode = http.GET();
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    updateStatus += "<br>HTTP Response Code: " + String(httpCode);

    if (httpCode != HTTP_CODE_OK) {
        Serial.println("Failed to check version!");
        updateStatus += "<br>Failed to check version!";
        http.end();
        return;
    }

    // Stream JSON Parsing (Reduces Memory Usage)
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();  // Free memory **before** downloading firmware

    if (error) {
        Serial.println("JSON Parse Failed!");
        updateStatus += "<br>JSON Parse Failed!";
        return;
    }

    // Extract version dynamically
    String latest_version = doc["tag_name"].as<String>();
    String firmware_url = doc["assets"][0]["browser_download_url"].as<String>();

    Serial.printf("Latest: %s | Current: %s\n", latest_version.c_str(), current_version.c_str());
    updateStatus += "<br>Latest version: " + latest_version;
    updateStatus += "<br>Current version: " + current_version;

    // **Update the `current_version` to match the latest tag**
    if (!latest_version.isEmpty()) {
        current_version = latest_version;
    }

    if (latest_version == VERSION) {  // Compare with `VERSION`
        Serial.println("Already up to date.");
        updateStatus += "<br>Already up to date.";
        return;
    }

    Serial.println("New version found! Starting update...");
    updateStatus += "<br>New version found! Starting update...";

    // Download Firmware
    http.begin(*client, firmware_url);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    int firmwareHttpCode = http.GET();

    if (firmwareHttpCode != HTTP_CODE_OK) {
        Serial.printf("Firmware download failed! HTTP Error: %d\n", firmwareHttpCode);
        updateStatus += "<br>Firmware download failed!";
        http.end();
        return;
    }

    WiFiClient& stream = http.getStream();
    size_t contentLength = http.getSize();

    if (contentLength <= 0) {
        Serial.println("Invalid firmware size!");
        updateStatus += "<br>Invalid firmware size!";
        http.end();
        return;
    }

    Serial.printf("Firmware Size: %d bytes\n", contentLength);
    updateStatus += "<br>Firmware Size: " + String(contentLength) + " bytes";

    if (!Update.begin(contentLength)) {
        Serial.println("Not enough space for update!");
        updateStatus += "<br>Not enough space for update!";
        http.end();
        return;
    }

    // **Download in Chunks to Avoid OOM**
    Serial.println("Updating firmware...");
    updateStatus += "<br>Updating firmware...";
    uint8_t buff[512]; // Small buffer (Adjustable: 512 - 1024)
    size_t written = 0;

    while (written < contentLength) {
        size_t available = stream.available();
        if (available) {
            size_t chunkSize = min(available, sizeof(buff));
            int bytesRead = stream.readBytes(buff, chunkSize);
            if (bytesRead > 0) {
                Update.write(buff, bytesRead);
                written += bytesRead;
                Serial.printf("Progress: %d/%d bytes\n", written, contentLength);
            }
        }
    }

    if (!Update.end()) {
        Serial.printf("Update failed! Error: %s\n", Update.getErrorString().c_str());
        updateStatus += "<br>Update failed!";
    } else {
        Serial.println("Update complete! Rebooting...");
        updateStatus += "<br>Update complete! Rebooting...";
        ESP.restart();
    }

    http.end();
}

String getHTML() {
    String html = "<!DOCTYPE HTML><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 0;";
    html += "background-image: url('https://wallpapersok.com/images/high/illustration-of-millenium-falcon-in-star-wars-cell-phone-cesjxv8vam4s0sb5.webp'); ";
    html += "background-size: cover; background-position: center; display: flex; flex-direction: column; height: 100vh; justify-content: space-between; }";
    html += "h1 { color: black; margin: 20px; text-shadow: 2px 2px 5px #000; }";
    html += ".button { margin: 10px; width: 120px; height: 50px; font-size: 16px; color: white; border: none; border-radius: 10px; cursor: pointer; display: inline-block; }";
    html += ".on { background-color: green; }";
    html += ".off { background-color: red; }";
    html += ".button:active { transform: scale(0.95); }";
    html += ".reboot { background-color: lightgreen; color: black; }";
    html += ".reboot:hover { background-color: #90ee90; }";
    html += ".button-container { display: flex; flex-wrap: wrap; justify-content: center; margin-top: 20px; }";
    html += ".button-container .button { margin: 10px; }";
    html += "</style></head><body>";

    html += "<h1>ESP8266 Web Server WITH OTA " + current_version + "</h1>";
    html += "<p>LED state: <strong id='ledState' style='color: red;'>" + String(LED_state ? "ON" : "OFF") + "</strong></p>";

    html += "<div class='button-container'>";
    html += "<button class='button " + String(LED_state ? "on" : "off") + "' onclick=\"sendRequest('/led1/on')\">Turn ON</button>";
    html += "<button class='button " + String(!LED_state ? "on" : "off") + "' onclick=\"sendRequest('/led1/off')\">Turn OFF</button>";
    html += "</div>";

    html += "<div class='button-container'>";
    html += "<button class='button reboot' onclick=\"sendRequest('/reboot')\">Reboot</button>";
    html += "<button class='button' onclick=\"sendRequest('/update')\">Check for Update</button>";
    html += "<button class='button' onclick=\"sendRequest('/apmode')\">Force AP Mode</button>";
    html += "</div>";

    html += "<div class='schedule'>";
    html += "<h3>LED Schedule</h3>";
    html += "<ul>";
    html += "<li>Morning ON: " + String(morningOnHour) + ":" + String(morningOnMinute < 10 ? "0" : "") + String(morningOnMinute) + "</li>";
    html += "<li>Morning OFF: " + String(morningOffHour) + ":" + String(morningOffMinute < 10 ? "0" : "") + String(morningOffMinute) + "</li>";
    html += "<li>Evening OFF: " + String(eveningOffHour) + ":" + String(eveningOffMinute < 10 ? "0" : "") + String(eveningOffMinute) + "</li>";
    html += "<li>Sunset Time: " + String(getSunsetHour()) + ":00</li>";
    html += "</ul></div>";

    html += "<a href='/viewlog' style='display:block;margin:20px;'>View Wi-Fi Log</a>";

    html += "<h3 style='text-align: center;'>WiFi Configuration</h3>";
    html += "<div class='wifi-container'>";
    html += "  <form method='post' action='/setwifi' class='wifi-form'>";
    html += "    <label>SSID: <input type='text' name='ssid'></label>";
    html += "    <label>Password: <input type='password' name='password'></label>";
    html += "    <input type='submit' value='Save' style='margin-top: 10px;'>";
    html += "  </form>";
    html += "</div>";

    html += "<script>";
    html += "function sendRequest(url) {";
    html += "  var xhr = new XMLHttpRequest();";
    html += "  xhr.open('GET', url, true);";
    html += "  xhr.onload = function() {";
    html += "    if (xhr.status == 200) {";
    html += "      if (url.includes('led1')) {";
    html += "        document.getElementById('ledState').innerText = url.includes('on') ? 'ON' : 'OFF';";
    html += "      }";
    html += "    }";
    html += "  };";
    html += "  xhr.send();";
    html += "}";
    html += "</script>";
    html += "</body></html>";

    return html;
}

void setupRoutes() {
    // Serve the main webpage
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", getHTML());
    });

    // Handle form submission for /setwifi
    server.on("/setwifi", HTTP_POST, []() {
        ssid = server.arg("ssid");
        password = server.arg("password");
        saveConfig();
        server.send(200, "text/plain", "WiFi credentials saved. Rebooting...");
        delay(1000);
        ESP.restart();
    });

    // Other routes
    server.on("/led1/on", HTTP_GET, []() {
        LED_state = HIGH;
        digitalWrite(LED_PIN, LED_state);
        Serial.println("LED turned ON");
        server.send(200, "text/plain", "ON");
    });

    server.on("/led1/off", HTTP_GET, []() {
        LED_state = LOW;
        digitalWrite(LED_PIN, LED_state);
        Serial.println("LED turned OFF");
        server.send(200, "text/plain", "OFF");
    });

    server.on("/reboot", HTTP_GET, []() {
        rebootStatus = "Rebooting...";
        server.send(200, "text/plain", "Rebooting...");
        delay(1000);
        ESP.restart();
    });

    server.on("/update", HTTP_GET, []() {
        checkForUpdates();
        server.send(200, "text/plain", updateStatus);
    });

    server.on("/apmode", HTTP_GET, []() {
        server.send(200, "text/plain", "Forcing AP Mode...");
        delay(1000);
        startAPMode();
    });
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, LOW);  // Initialize STATUS_LED

    if (!LittleFS.begin()) {
        Serial.println("Failed to mount LittleFS!");
        return;
    } else {
        Serial.println("LittleFS mounted successfully.");
    }

    if (!LittleFS.exists("/config.json")) {
        Serial.println("Config file missing! Starting AP mode...");
        startAPMode();
    } else {
        loadConfig();
        connectWiFi();
    }

    startOTA();
    timeClient.begin();

    // Always setup the routes first
    setupRoutes();
    server.begin(); // Start the web server
}

void loop() {
    server.handleClient();  // Always handle requests

    ArduinoOTA.handle();
    timeClient.update();

    // Handle STATUS_LED blinking based on WiFi mode
    unsigned long currentMillis = millis();
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(STATUS_LED, LOW);  // Constant on when connected
    } else if (WiFi.getMode() == WIFI_AP) {
        // Slow blink in AP mode
        if (currentMillis - previousMillis >= apBlinkInterval) {
            previousMillis = currentMillis;
            statusLedState = !statusLedState;
            digitalWrite(STATUS_LED, statusLedState);
        }
    } else {
        // Fast blink when trying to connect
        if (currentMillis - previousMillis >= wifiBlinkInterval) {
            previousMillis = currentMillis;
            statusLedState = !statusLedState;
            digitalWrite(STATUS_LED, statusLedState);
        }
    }

    // Button handling logic here
    int reading = digitalRead(SWITCH_PIN);
    if (reading != lastSwitchState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading != switchState) {
            switchState = reading;
            if (switchState == LOW) {
                LED_state = !LED_state;
                digitalWrite(LED_PIN, LED_state);
                server.send(200, "text/html", getHTML());
            }
        }
    }
    lastSwitchState = reading;

    // Check if it's reboot time
    if (timeClient.getHours() == rebootHour && timeClient.getMinutes() == rebootMinute) {
        if (!rebootTriggered) {
            Serial.println("Reboot time reached. Rebooting...");
            rebootTriggered = true;
            ESP.restart();
        }
    } else {
        rebootTriggered = false;
    }

    int currentHour = timeClient.getHours();
    int currentMinute = timeClient.getMinutes();

    if (currentHour == morningOnHour && currentMinute == morningOnMinute) {
        digitalWrite(LED_PIN, HIGH);
        LED_state = HIGH;
        Serial.println("LED turned ON (morning schedule)");
    }
    if (currentHour == morningOffHour && currentMinute == morningOffMinute) {
        digitalWrite(LED_PIN, LOW);
        LED_state = LOW;
        Serial.println("LED turned OFF (morning schedule)");
    }

    int sunsetHour = getSunsetHour();
    if (currentHour == (sunsetHour - 1) && currentMinute == 0) {
        digitalWrite(LED_PIN, HIGH);
        LED_state = HIGH;
        Serial.println("LED turned ON (evening schedule)");
    }
    if (currentHour == eveningOffHour && currentMinute == eveningOffMinute) {
        digitalWrite(LED_PIN, LOW);
        LED_state = LOW;
        Serial.println("LED turned OFF (evening schedule)");
    }

    if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP) {
        Serial.println("Lost WiFi connection! Starting AP mode...");
        startAPMode();
    }
}