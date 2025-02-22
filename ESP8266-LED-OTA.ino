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

#define LED_PIN D6         // The ESP8266 pin connected to LED
#define SWITCH_PIN D5      // The ESP8266 pin connected to the momentary switch
#define STATUS_LED D4      // Status LED for WiFi connection feedback

const String current_version = "1.0.1";  // Change this to match your current firmware version
const char* version_url = "https://raw.githubusercontent.com/SWhardfish/ESP8266-LED-OTA/main/version.txt";  // URL to version file
const char *firmware_url = "https://raw.githubusercontent.com/SWhardfish/ESP8266-LED-OTA/main/ESP8266-LED-OTA.bin"; // Change this to your hosted firmware binary

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

void checkForUpdates() {
    Serial.println("Checking for firmware updates...");

    WiFiClientSecure client;
    HTTPClient http;

    Serial.print("Connecting to: ");
    Serial.println(version_url);

    // Use WiFiClientSecure to make the request
    client.setInsecure();  // Use this to bypass certificate verification (not recommended for production)
    http.begin(client, version_url);  // Pass the secure client to HTTPClient

    int httpCode = http.GET();
    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);

    if (httpCode == HTTP_CODE_OK) {
        String latest_version = http.getString();
        latest_version.trim(); // Clean up response

        Serial.print("Latest version: ");
        Serial.println(latest_version);
        Serial.print("Current version: ");
        Serial.println(current_version);

        if (latest_version != current_version) {
            Serial.println("New version available! Updating...");
            t_httpUpdate_return result = ESPhttpUpdate.update(client, firmware_url);

            switch (result) {
                case HTTP_UPDATE_FAILED:
                    Serial.printf("Update failed! Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
                    break;
                case HTTP_UPDATE_NO_UPDATES:
                    Serial.println("No new update available.");
                    break;
                case HTTP_UPDATE_OK:
                    Serial.println("Update successful! Rebooting...");
                    delay(1000);
                    ESP.restart();
                    break;
            }
        } else {
            Serial.println("Firmware is up to date.");
        }
    } else {
        Serial.printf("Failed to check version! HTTP error: %d\n", httpCode);
    }

    http.end();
}


String getHTML() {
    String html = "<!DOCTYPE HTML><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{text-align:center;font-family:Arial;}"; 
    html += ".button{padding:10px 20px;font-size:18px;display:inline-block;margin:10px;border:none;background:blue;color:white;cursor:pointer;}"; 
    html += "</style></head><body>";

    html += "<h2>ESP8266 Web Server WITH OTA ***</h2>";
    html += "<p>LED state: <strong style='color: red;'>";
    html += (LED_state == LOW) ? "OFF" : "ON";
    html += "</strong></p>";
    html += "<a class='button' href='/led1/on'>Turn ON</a>";
    html += "<a class='button' href='/led1/off'>Turn OFF</a>";
    html += "<a class='button' href='/reboot'>Reboot</a>";
    html += "<a class='button' href='/update'>Check for Update</a>";

    html += "<p>Current Time: " + timeClient.getFormattedTime() + "</p>";
    html += "</body></html>";

    return html;
}

void setup() {
    Serial.begin(115200);
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
        server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
        delay(1000);
        ESP.restart();
    });

    server.on("/update", HTTP_GET, []() {
        server.send(200, "text/html", "<html><body><h1>Checking for updates...</h1></body></html>");
        checkForUpdates();
    });


    server.begin();
}

void loop() {
    server.handleClient();
    ArduinoOTA.handle();
    timeClient.update(); // Update NTP time

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

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Lost WiFi connection! Reconnecting...");
        connectWiFi();
    }
}
