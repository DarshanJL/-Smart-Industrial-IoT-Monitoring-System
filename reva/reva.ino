/**
 * Industrial Monitoring System with LVGL UI, MQTT, and SD Card Logging
 */
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "ui.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "aws_certs.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <ESP32Time.h>
#include <HTTPClient.h>

// WiFi Configuration
const char* ssid = "OPPOF";
const char* password = "darshan@123";

// AWS IoT Configuration
const char* awsEndpoint = "add_awsEndpoint";
const int awsPort = 8883;
const char* mqttTopic = "REVA/INDUSTRIAL_MONITORING_SYSTEM/";

// S3 Configuration
const char* s3_base_url = "https://convin-audio-files.s3.ap-south-1.amazonaws.com/";

// SD Card Configuration
const int SD_CS = 5;
const int SD_MOSI = 23;
const int SD_MISO = 19;
const int SD_SCK = 18;

// Time Configuration
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 19800;  // UTC+5:30 = 5.5 * 3600 = 19800 seconds
const int daylightOffset_sec = 0;  // No daylight saving time in India
ESP32Time rtc;  // Software RTC

// System Status
bool sdCardAvailable = false;
bool awsConnected = false;

WiFiClientSecure net;
PubSubClient mqttClient(net);
String clientId = "convin-aws-" + String(random(0xffff), HEX);

// Timing variables
unsigned long lastUploadTime = 0;
const unsigned long uploadInterval = 5 * 60 * 1000; // 5 minutes in milliseconds
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 5000; // 5 seconds
unsigned long lastDisplayUpdate = 0;
const unsigned long displayUpdateInterval = 1000; // Update display every second

// Sensor data storage
struct SensorData {
  float temperature = 0.0;
  float humidity = 0.0;
  int vibration = 0;
  float dht_temperature = 0.0;
  String fan_status = "OFF";
  String system_status = "0%";
} currentData;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  
  // Initialize display
  LCD_Init();
  Set_Backlight(90);
  Lvgl_Init();
  ui_init();
  
  // Initialize hardware
  initializeSDCard();
  connectToWiFi();
  
  // Initialize time sync
  if (!syncTimeWithNTP()) {
    Serial.println("Failed to sync with NTP, using compile time");
    rtc.setTime(0); // This will set to compile time
  }
  
  // Setup AWS IoT
  setupAWSIoT();
  
  if (mqttClient.connected()) {
    subscribeToTopics();
  }
  
  // Initial display update
  updateDisplay();
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    if (currentMillis - lastReconnectAttempt >= reconnectInterval) {
      lastReconnectAttempt = currentMillis;
      reconnectAWSIoT();
    }
  } else {
    mqttClient.loop();
  }
  
  // Check for upload every 5 minutes
  if (currentMillis - lastUploadTime >= uploadInterval) {
    lastUploadTime = currentMillis;
    if (sdCardAvailable) {
      uploadFilesToS3();
    } else {
      Serial.println("Skipping upload - SD card not available");
      initializeSDCard(); // Attempt to reinitialize
    }
  }
  
  // Update display every second
  if (currentMillis - lastDisplayUpdate >= displayUpdateInterval) {
    lastDisplayUpdate = currentMillis;
    updateDisplay();
  }
  
  // Periodically check SD card health
  static unsigned long lastSDCheck = 0;
  if (currentMillis - lastSDCheck >= 60000) { // Every minute
    lastSDCheck = currentMillis;
    checkSDCardHealth();
  }
  
  lv_timer_handler();
  delay(5);
}

void updateDisplay() {
  // Update time and date
  lv_label_set_text(ui_Label4, rtc.getTime("%I:%M%p").c_str());
  lv_label_set_text(ui_Label6, rtc.getTime("%d-%m-%Y").c_str());
  
  // Update sensor data
  lv_label_set_text(ui_Label11, String(currentData.temperature, 1).c_str());
  lv_label_set_text(ui_Label12, String(currentData.humidity, 0).c_str());
  lv_label_set_text(ui_Label13, String(currentData.vibration).c_str());
  lv_label_set_text(ui_Label14, currentData.system_status.c_str());
  lv_label_set_text(ui_Label10, currentData.fan_status.c_str());
  
  // Update arc values
  lv_arc_set_value(ui_Arc1, map(currentData.temperature, 0, 50, 0, 360));
  lv_arc_set_value(ui_Arc3, map(currentData.humidity, 0, 100, 0, 360));
  lv_arc_set_value(ui_Arc4, map(currentData.vibration, 0, 100, 0, 360));
  
  // System status (convert percentage string to integer)
  int systemPercent = currentData.system_status.toInt();
  lv_arc_set_value(ui_Arc5, map(systemPercent, 0, 100, 0, 360));
}

void initializeSDCard() {
  Serial.println("\nInitializing SD card...");
  
  // SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(100); // Short delay for stability
  
  // Try multiple times to initialize
  for (int attempt = 0; attempt < 3; attempt++) {
      sdCardAvailable = true;
      Serial.println("SD Card initialized successfully");
      
      // Print card info
      uint8_t cardType = SD.cardType();
      Serial.print("Card Type: ");
      switch(cardType) {
        case CARD_NONE: Serial.println("No card attached"); break;
        case CARD_MMC: Serial.println("MMC"); break;
        case CARD_SD: Serial.println("SDSC"); break;
        case CARD_SDHC: Serial.println("SDHC"); break;
        default: Serial.println("Unknown");
      }
      
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("Card Size: %lluMB\n", cardSize);
      return;
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nSD Card initialization failed!");
  sdCardAvailable = false;
}

void checkSDCardHealth() {
  if (!sdCardAvailable) {
    Serial.println("SD card not available, attempting reinitialization...");
    initializeSDCard();
    return;
  }
  
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD card lost!");
    sdCardAvailable = false;
    return;
  }
  
  // Test write capability
  String testFile = "/healthcheck_" + String(millis()) + ".tmp";
  if (!writeToFile(testFile, "Health check " + String(millis()))) {
    Serial.println("SD card health check failed!");
    sdCardAvailable = false;
    return;
  }
  
  if (!SD.remove(testFile)) {
    Serial.println("Warning: Couldn't delete test file");
  }
  
  Serial.println("SD card health check passed");
}

bool writeToFile(String filename, String content) {
  // First ensure the directory exists
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash > 0) {
    String directory = filename.substring(0, lastSlash);
    if (!ensureDirectoryExists(directory)) {
      Serial.println("Failed to create directory for: " + filename);
      return false;
    }
  }
  
  // Now write the file
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing: " + filename);
    return false;
  }
  
  size_t bytesWritten = file.print(content);
  file.close();
  
  if (bytesWritten != content.length()) {
    Serial.println("Incomplete write to file: " + filename);
    return false;
  }
  
  return true;
}

bool ensureDirectoryExists(String path) {
  // Start from the root directory
  String currentPath = "";
  
  // Handle absolute paths correctly
  if (path.startsWith("/")) {
    currentPath = "/";
  }
  
  // Split the path by '/'
  int startIndex = 0;
  while (startIndex < path.length()) {
    int endIndex = path.indexOf('/', startIndex);
    if (endIndex == -1) {
      endIndex = path.length();
    }
    
    String segment = path.substring(startIndex, endIndex);
    if (segment.length() > 0) {
      if (currentPath.length() > 0 && !currentPath.endsWith("/")) {
        currentPath += "/";
      }
      currentPath += segment;
      
      if (!SD.exists(currentPath)) {
        Serial.println("Creating directory: " + currentPath);
        if (!SD.mkdir(currentPath)) {
          Serial.println("Failed to create directory: " + currentPath);
          return false;
        }
      }
    }
    
    startIndex = endIndex + 1;
  }
  
  return true;
}

void connectToWiFi() {
  Serial.print("\nConnecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.disconnect(true);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nFailed to connect to WiFi");
    ESP.restart();
  }
  
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

bool syncTimeWithNTP() {
  Serial.println("Syncing time with NTP servers...");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("Failed to obtain NTP time");
    return false;
  }
  
  time_t now;
  time(&now);
  rtc.setTime(now);
  
  Serial.print("Time synchronized: ");
  Serial.println(rtc.getTime("%Y-%m-%d %H:%M:%S"));
  return true;
}

void setupAWSIoT() {
  Serial.println("\nConfiguring AWS IoT connection...");
  
  net.setCACert(awsRootCA);
  net.setCertificate(awsClientCert);
  net.setPrivateKey(awsPrivateKey);

  mqttClient.setServer(awsEndpoint, awsPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  
  reconnectAWSIoT();
}

void reconnectAWSIoT() {
  Serial.println("Attempting to connect to AWS IoT...");
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, reconnecting...");
    connectToWiFi();
    delay(1000);
    return;
  }

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("AWS IoT Connected!");
    awsConnected = true;
    subscribeToTopics();
  } else {
    Serial.print("AWS IoT Connection Failed, rc=");
    Serial.println(mqttClient.state());
    awsConnected = false;
  }
}

void subscribeToTopics() {
  bool sub1 = mqttClient.subscribe(mqttTopic);
  Serial.printf("Subscription to %s %s\n", mqttTopic, sub1 ? "successful" : "failed");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("\nMessage arrived [");
  Serial.print(topic);
  Serial.print("] Length: ");
  Serial.println(length);
  
  // Create null-terminated string
  char* message = new char[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  
  Serial.print("Payload: ");
  Serial.println(message);

  // Parse JSON
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    delete[] message;
    return;
  }
  
  // Update sensor data
  currentData.temperature = doc["temperature"] | 0.0;
  currentData.humidity = doc["humidity"] | 0.0;
  currentData.vibration = doc["vibration_count"] | 0;
  currentData.dht_temperature = doc["dht_temperature"] | 0.0;
  currentData.fan_status = doc["FAN"] | "OFF";
  currentData.system_status = doc["System"] | "0%";
  
  // Update display immediately
  updateDisplay();

  if (!sdCardAvailable) {
    Serial.println("SD card not available, skipping save operation");
    delete[] message;
    initializeSDCard(); // Attempt to reinitialize
    return;
  }

  // Get current date and time for filename
  String timestamp = rtc.getTime("%Y-%m-%d_%H-%M-%S");
  String dateFolder = rtc.getTime("/%Y-%m-%d");
  String filename = dateFolder + "/data_" + timestamp + ".json";
  
  // Save to SD card
  if (writeToFile(filename, message)) {
    Serial.println("Successfully saved to: " + filename);
  } else {
    Serial.println("Failed to save message to SD card");
  }
  
  delete[] message;
}

void uploadFilesToS3() {
  if (!sdCardAvailable) {
    Serial.println("Skipping upload - SD card not available");
    return;
  }
  
  Serial.println("\nStarting S3 upload process...");
  
  // Get current date for folder
  String todayFolder = rtc.getTime("/%Y-%m-%d");
  
  if (!SD.exists(todayFolder)) {
    Serial.println("No data folder found for today");
    return;
  }
  
  File dir = SD.open(todayFolder);
  if (!dir) {
    Serial.println("Failed to open directory");
    return;
  }
  
  int filesUploaded = 0;
  while (File entry = dir.openNextFile()) {
    if (!entry.isDirectory() && String(entry.name()).endsWith(".json")) {
      String filename = todayFolder + "/" + entry.name();
      String s3_url = String(s3_base_url) + filename.substring(1);
      
      Serial.print("Uploading ");
      Serial.print(filename);
      Serial.print(" to ");
      Serial.println(s3_url);
      
      // Read file content
      String fileContent;
      while (entry.available()) {
        fileContent += (char)entry.read();
      }
      entry.close();
      
      // Upload to S3
      HTTPClient http;
      http.begin(s3_url);
      http.addHeader("Content-Type", "application/json");
      
      int httpResponseCode = http.PUT(fileContent);
      if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);
        
        if (httpResponseCode == 200) {
          if (SD.remove(filename)) {
            Serial.println("File deleted after successful upload");
            filesUploaded++;
          } else {
            Serial.println("Error deleting file");
          }
        }
      } else {
        Serial.print("Error on HTTP request: ");
        Serial.println(httpResponseCode);
      }
      http.end();
    }
  }
  dir.close();
  
  Serial.printf("Upload complete. Files uploaded: %d\n", filesUploaded);
}
