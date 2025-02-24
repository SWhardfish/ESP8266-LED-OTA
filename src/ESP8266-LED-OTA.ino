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

#define DEBUG_ESP_HTTP_UPDATE
#define DEBUG_ESP_PORT Serial

#define LED_PIN D6         // The ESP8266 pin connected to LED
#define SWITCH_PIN D5      // The ESP8266 pin connected to the momentary switch
#define STATUS_LED D4      // Status LED for WiFi connection feedback

const String current_version = "v1.0.26";  // Set this to the current version of your firmware
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

// Simplified sunset time for Stockholm (approximation)
int getSunsetHour() {
    // Use a fixed sunset time for simplicity (can be adjusted seasonally)
    return 17; // 9 PM (summer sunset time)
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

    // Read file and parse JSON
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
    } else {
        Serial.println("\nFailed to connect. Restarting...");
        ESP.restart();
    }

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
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

    // Remove previous OTA file if exists
    removePreviousOTAFile();

    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
    HTTPClient http;

    client->setInsecure();  // Bypass SSL certificate verification
    http.begin(*client, api_url);
    http.setTimeout(20000);  // 20-second timeout
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // Ensure redirect handling

    int httpCode = http.GET();
    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    updateStatus += "<br>HTTP Response Code: " + String(httpCode);

    if (httpCode == HTTP_CODE_OK) {
        // Parse JSON response
        String payload = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.println("Failed to parse JSON");
            updateStatus += "<br>Failed to parse JSON";
            return;
        }

        // Extract latest version and firmware URL
        String latest_version = doc["tag_name"].as<String>();
        String firmware_url = doc["assets"][0]["browser_download_url"].as<String>();

        Serial.print("Latest version: ");
        Serial.println(latest_version);
        Serial.print("Current version: ");
        Serial.println(current_version);
        updateStatus += "<br>Latest version: " + latest_version;
        updateStatus += "<br>Current version: " + current_version;

        if (latest_version != current_version) {
            Serial.println("New version available! Updating...");
            Serial.print("Firmware URL: ");
            Serial.println(firmware_url);
            updateStatus += "<br>New version available! Updating...";
            updateStatus += "<br>Firmware URL: " + firmware_url;

            // Manually download firmware
            http.end();
            http.begin(*client, firmware_url);
            http.setTimeout(20000);  // Set timeout for firmware download

            int firmwareHttpCode = http.GET();
            if (firmwareHttpCode == HTTP_CODE_OK) {
                WiFiClient& stream = http.getStream();  // Corrected type
                size_t contentLength = http.getSize();
                if (contentLength > 0) {
                    Serial.printf("Firmware size: %d bytes\n", contentLength);
                    updateStatus += "<br>Firmware size: " + String(contentLength) + " bytes";

                    // Start OTA update
                    if (Update.begin(contentLength)) {
                        Serial.println("Starting OTA update...");
                        updateStatus += "<br>Starting OTA update...";
                        size_t written = Update.writeStream(stream);
                        if (written == contentLength) {
                            Serial.println("Firmware successfully written, finishing update...");
                            updateStatus += "<br>Firmware successfully written, finishing update...";
                            if (Update.end()) {
                                Serial.println("Update complete! Rebooting...");
                                updateStatus += "<br>Update complete! Rebooting...";
                                ESP.restart();
                            } else {
                                Serial.printf("Update failed! Error: %s\n", Update.getErrorString().c_str());
                                updateStatus += "<br>Update failed! Error: " + String(Update.getErrorString().c_str());
                            }
                        } else {
                            Serial.printf("Firmware write failed! Only wrote %d of %d bytes\n", written, contentLength);
                            updateStatus += "<br>Firmware write failed! Only wrote " + String(written) + " of " + String(contentLength) + " bytes";
                        }
                    } else {
                        Serial.println("Not enough space for OTA update.");
                        updateStatus += "<br>Not enough space for OTA update.";
                    }
                } else {
                    Serial.println("Firmware download error: Empty response");
                    updateStatus += "<br>Firmware download error: Empty response";
                }
            } else {
                Serial.printf("Failed to download firmware! HTTP Error: %d\n", firmwareHttpCode);
                updateStatus += "<br>Failed to download firmware! HTTP Error: " + String(firmwareHttpCode);
            }
        } else {
            Serial.println("Firmware is up to date.");
            updateStatus += "<br>Firmware is up to date.";
        }
    } else {
        Serial.printf("Failed to check version! HTTP error: %d\n", httpCode);
        updateStatus += "<br>Failed to check version! HTTP error: " + String(httpCode);
    }

    http.end();
}

String getHTML() {
    String html = "<!DOCTYPE HTML><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{text-align:center;font-family:Arial;}";
    html += ".button{padding:10px 20px;font-size:18px;display:inline-block;margin:10px;border:none;background:blue;color:white;cursor:pointer;border-radius:10px;}";
    html += ".button-container{display:flex;flex-wrap:wrap;justify-content:center;}";
    html += ".button-container a{flex:1 1 45%;margin:5px;}";
    html += ".schedule{text-align:left;margin:20px auto;width:80%;max-width:400px;padding:10px;border:1px solid #ccc;border-radius:10px;}";
    html += ".schedule h3{margin:0 0 10px 0;}";
    html += ".schedule ul{list-style-type:none;padding:0;}";
    html += ".schedule li{margin:5px 0;}";
    html += "</style></head><body>";

    html += "<h2>ESP8266 Web Server WITH OTA " + current_version + "</h2>";
    html += "<p>LED state: <strong style='color: red;'>";
    html += (LED_state == LOW) ? "OFF" : "ON";
    html += "</strong></p>";
    html += "<div class='button-container'>";
    html += "<a class='button' href='/led1/on'>Turn ON</a>";
    html += "<a class='button' href='/led1/off'>Turn OFF</a>";
    html += "</div>";
    html += "<div class='button-container'>";
    html += "<a class='button' href='/reboot'>Reboot</a>";
    html += "<a class='button' href='/update'>Check for Update</a>";
    html += "</div>";
    html += "<div class='schedule'>";
    html += "<h3>Reboot Schedule</h3>";
    html += "<ul>";
    html += "<li>Reboot Time: " + String(rebootHour) + ":" + String(rebootMinute < 10 ? "0" : "") + String(rebootMinute) + "</li>";
    html += "</ul>";
    html += "<h3>LED Schedule</h3>";
    html += "<ul>";
    html += "<li>Morning ON: " + String(morningOnHour) + ":" + String(morningOnMinute < 10 ? "0" : "") + String(morningOnMinute) + "</li>";
    html += "<li>Morning OFF: " + String(morningOffHour) + ":" + String(morningOffMinute < 10 ? "0" : "") + String(morningOffMinute) + "</li>";
    html += "<li>Evening OFF: " + String(eveningOffHour) + ":" + String(eveningOffMinute < 10 ? "0" : "") + String(eveningOffMinute) + "</li>";
    html += "</ul>";
    html += "</div>";
    html += "<p>Current Time: " + timeClient.getFormattedTime() + "</p>";
    html += "<p>" + updateStatus + "</p>";
    html += "<p>" + rebootStatus + "</p>";
    html += "</body></html>";

    return html;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Step 1: Check if LittleFS mounts correctly
    if (!LittleFS.begin()) {
        Serial.println("Failed to mount LittleFS!");
        return;  // Stop execution if filesystem is not available
    } else {
        Serial.println("LittleFS mounted successfully.");
    }

    // Step 2: Check if config.json exists before opening
    if (!LittleFS.exists("/config.json")) {
        Serial.println("Config file missing!");
    } else {
        Serial.println("Config file found. Reading...");

        // Now try to open and read the config file
        File configFile = LittleFS.open("/config.json", "r");
        if (!configFile) {
            Serial.println("Failed to open config file.");
        } else {
            Serial.println("Config file opened successfully.");
            String configData = configFile.readString();
            Serial.println("Config Contents: " + configData);
            configFile.close();
        }
    }
    pinMode(LED_PIN, OUTPUT);
    pinMode(SWITCH_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, HIGH); // Start with status LED on

    loadConfig();  // Load WiFi credentials from file
    connectWiFi();
    startOTA();
    timeClient.begin();

    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", getHTML());
    });

    server.on("/led1/on", HTTP_GET, []() {
        LED_state = HIGH;
        digitalWrite(LED_PIN, LED_state);
        Serial.println("LED turned ON");
        server.send(200, "text/html", getHTML());
    });

    server.on("/led1/off", HTTP_GET, []() {
        LED_state = LOW;
        digitalWrite(LED_PIN, LED_state);
        Serial.println("LED turned OFF");
        server.send(200, "text/html", getHTML());
    });

    server.on("/reboot", HTTP_GET, []() {
        rebootStatus = "Rebooting...";
        server.send(200, "text/html", getHTML());
        delay(1000);
        ESP.restart();
    });

    server.on("/update", HTTP_GET, []() {
        checkForUpdates();
        server.send(200, "text/html", getHTML());
    });

    server.begin();
}

void loop() {
    server.handleClient();
    ArduinoOTA.handle();
    timeClient.update(); // Update NTP time

    // Handle momentary switch
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

    // Handle daily reboot
    if (timeClient.getHours() == rebootHour && timeClient.getMinutes() == rebootMinute) {
        if (!rebootTriggered) {
            Serial.println("Reboot time reached. Rebooting...");
            rebootTriggered = true;
            ESP.restart();
        }
    } else {
        rebootTriggered = false;
    }

    // Handle LED schedule
    int currentHour = timeClient.getHours();
    int currentMinute = timeClient.getMinutes();

    // Morning schedule: Turn on at 06:30, turn off at 09:00
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

    // Evening schedule: Turn on 1 hour before sunset, turn off at 23:30
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

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Lost WiFi connection! Reconnecting...");
        connectWiFi();
    }
}