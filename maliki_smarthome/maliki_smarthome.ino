#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h> 

unsigned long previousMillis = 0;
const long interval = 1000;  // Update interval in milliseconds

// Network credentials
// Remember to fill your SSID and WiFi password
const char* ssid = "......";
const char* password = "......";

// ThingsBoard device token
// Remember to fill your ThingsBoard token
const char* token = "......"; 

// DHT setup
#define DHTPIN 23 // ESP32 pin connected to DHT22 data pin
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// PIR setup
#define PIRPIN 19 // ESP32 pin connected to PIR sensor's output pin

// MQ2 setup
#define MQ2PIN 34 // ESP32 pin connected to MQ2 sensor's analog output

// LED setup
#define LEDPIN 17 // ESP32 pin connected to LED (for HVAC simulation)

// OLED display setup
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Comfortable range constants
const float COMFORTABLE_TEMP_LOW = 20.0; // 20 degrees Celsius
const float COMFORTABLE_TEMP_HIGH = 28.5; // 27 degrees Celsius
const float COMFORTABLE_HUM_LOW = 40.0; // 40% RH
const float COMFORTABLE_HUM_HIGH = 57.0; // 60% RH

bool hvacStatus = false;
bool motionDetected = false;
bool gasLeak = false;

// Gas threshold for alarm
const int GAS_THRESHOLD = 300; // Example threshold value for the MQ2 sensor

WiFiClient wiFiClient;
PubSubClient client(wiFiClient);

String createJsonPayload(float temperature, float humidity, int pirValue, int mq2Value) {
  String payload = "{";
  payload += "\"temperature\":"; payload += String(temperature); payload += ",";
  payload += "\"humidity\":"; payload += String(humidity); payload += ",";
  payload += "\"motion\":"; payload += pirValue ? "true" : "false"; payload += ",";
  payload += "\"gas\":"; payload += String(mq2Value);
  payload += "}";
  return payload;
}

void updateAttributes() {
  // Prepare the attribute JSON payload
  String attributesPayload = "{\"hvacStatus\":" + String(hvacStatus) + ", \"gasLeak\":" + String(gasLeak) + "}";

  // Convert payload to character array
  char attributes[attributesPayload.length() + 1];
  attributesPayload.toCharArray(attributes, sizeof(attributes));

  // Publish the attributes to ThingsBoard
  client.publish("v1/devices/me/attributes", attributes);
}


String handleRPC(String payload) {
  Serial.println("Received RPC: " + payload);  // Log the received RPC payload
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print(F("deserializeJson() failed with code "));
    Serial.println(error.c_str());
    return "{\"error\":\"JSON parsing failed\"}";
  }
  
  String method = doc["method"];
  String params = doc["params"];
  Serial.println("RPC Method: " + method);  // Log the RPC method
  Serial.println("RPC Params: " + params);  // Log the RPC parameters
  
  // Initialize response JSON object
  DynamicJsonDocument respDoc(1024);
  String responsePayload;

  // Handling different RPC methods
  if (method == "setHVACStatus") {
    hvacStatus = (params == "true");
    digitalWrite(LEDPIN, hvacStatus ? HIGH : LOW);
    respDoc["hvacStatus"] = hvacStatus;
    Serial.println("HVAC status set to: " + String(hvacStatus));  // Log the action taken
  } else if (method == "checkMotionDetected") {
    motionDetected = (digitalRead(PIRPIN) == HIGH);
    respDoc["motionDetected"] = motionDetected;
    Serial.println("Motion detected: " + String(motionDetected));  // Log the motion status
  } else if (method == "checkGasLeak") {
    int mq2Value = analogRead(MQ2PIN);
    gasLeak = (mq2Value > GAS_THRESHOLD);
    respDoc["gasLeak"] = gasLeak;
    Serial.println("Gas leak status: " + String(gasLeak));  // Log the gas leak status
  } else {
    respDoc["error"] = "Unknown method";
    Serial.println("Received unknown RPC method");  // Log the error
  }

  // Serialize the JSON response
  serializeJson(respDoc, responsePayload);
  Serial.println("RPC Response: " + responsePayload);  // Log the response payload
  
  return responsePayload;
}


// Callback function for handling RPC requests
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);
  Serial.print("Payload length: ");
  Serial.println(length);
  Serial.print("Payload: ");
  
  // Print the payload as a string, not just as characters
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println(); // Print a newline

  // Convert topic and payload to string
  String topicStr = topic;
  String payloadStr = String((char*)payload).substring(0, length);

  // Print out the payload string for easier debugging
  Serial.print("Payload as String: ");
  Serial.println(payloadStr);

  // Parse the topic to determine the RPC method name and request ID
  int lastSlashIndex = topicStr.lastIndexOf('/');
  String requestId = topicStr.substring(lastSlashIndex + 1);

  // Handle the RPC command and get the response payload
  String responsePayload = handleRPC(payloadStr);

  // Print the response payload
  Serial.print("Response Payload: ");
  Serial.println(responsePayload);

  // Send a response back to the server
  String responseTopic = "v1/devices/me/rpc/response/" + requestId;
  client.publish(responseTopic.c_str(), responsePayload.c_str());
}


void handleHVAC(float temperature, float humidity) {
  if (temperature >= COMFORTABLE_TEMP_HIGH || humidity >= COMFORTABLE_HUM_HIGH) {
    digitalWrite(LEDPIN, HIGH);
    hvacStatus = true;
  } else {
  //else if (temperature < COMFORTABLE_TEMP_LOW || humidity < COMFORTABLE_HUM_LOW) {
    digitalWrite(LEDPIN, LOW);
    hvacStatus = false;
  }
    // Prepare the JSON string for publishing
  String hvacStatusJson = "{\"hvacStatus\":" + String(hvacStatus) + "}";
  // Send the HVAC status to ThingsBoard as telemetry data.
  client.publish("v1/devices/me/telemetry", hvacStatusJson.c_str());
}


void handleAlarms(int mq2Value, int pirValue) {
  
  String alarmMessage = createJsonPayload(0, 0, pirValue, mq2Value);
  char alarmAttributes[100];
  alarmMessage.toCharArray(alarmAttributes, 100);
  client.publish("v1/devices/me/attributes", alarmAttributes);

  // Display alarm on OLED
  display.clearDisplay();
  display.setCursor(0,0);
  display.print(F("ALARM!"));
  if (mq2Value > GAS_THRESHOLD) {
    display.println(F(" Gas Leak!"));
    client.publish("v1/devices/me/telemetry", "{\"gasDetected\":true}");
  } else {
    client.publish("v1/devices/me/telemetry", "{\"gasDetected\":false}");
  }
  if (pirValue) {
    display.println(F(" Motion Detected!"));
    client.publish("v1/devices/me/telemetry", "{\"motionDetected\":true}");
  } else {
    client.publish("v1/devices/me/telemetry", "{\"motionDetected\":false}");
  }
  display.display();
}

void updateOLED(float temperature, float humidity, int pirValue, int mq2Value) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.print(F("Smart Home Monitor\n"));
  display.print(F("Temp: "));
  display.print(temperature);
  display.println(" C");
  display.print(F("Humidity: "));
  display.print(humidity);
  display.println('%');
  display.print(F("Motion: "));
  display.println(pirValue ? F("Detected") : F("None"));
  display.print(F("Gas Level: "));
  display.println(mq2Value);
  display.print(F("MALIKI'S AUTO-HOME"));
  display.display();
}

void handleNetwork(float temperature, float humidity, int pirValue, int mq2Value) {
  if (WiFi.status() != WL_CONNECTED) {
    reconnectWiFi();
  }

  if (!client.connected()) {
    reconnectMQTT();
  } else {
    publishTelemetry(temperature, humidity, pirValue, mq2Value);
  }
}

void publishTelemetry(float temperature, float humidity, int pirValue, int mq2Value) {
  String payload = createJsonPayload(temperature, humidity, pirValue, mq2Value);
  Serial.print("Publishing telemetry: ");
  Serial.println(payload);
  char telemetry[100];
  payload.toCharArray(telemetry, 100);
  if (client.publish("v1/devices/me/telemetry", telemetry)) {
    Serial.println("Telemetry published successfully.");
    } 
  else {
    Serial.println("Telemetry publish failed.");
  }
}

void reconnectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print(F("Reconnecting to WiFi... "));
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 5000) {
    delay(500);
    Serial.print(F("."));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("Connected to WiFi"));
  } else {
    Serial.println(F("Failed to connect to WiFi"));
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print(F("Attempting MQTT connection..."));
    if (client.connect("ESP32Client", token, NULL)) {
      Serial.println(F("Connected to MQTT broker"));
      client.subscribe("v1/devices/me/rpc/request/+");
      Serial.println("Subscribed to RPC requests.");
    } else {
      Serial.print(F("Failed to connect to MQTT broker, rc="));
      Serial.print(client.state());
      Serial.println(F(". Trying again in 5 seconds."));
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LEDPIN, OUTPUT);
  dht.begin();
  pinMode(PIRPIN, INPUT);
  pinMode(MQ2PIN, INPUT);
  client.setServer("thingsboard.cloud", 1883);
  client.setCallback(callback);
  client.subscribe("v1/devices/me/rpc/request/+");
  reconnectWiFi();
  reconnectMQTT();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.display();
  delay(2000);
  display.clearDisplay();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi...");
  }


}

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    int pirValue = digitalRead(PIRPIN);
    int mq2Value = analogRead(MQ2PIN);

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println(F("Failed to read from DHT sensor!"));
      delay(1000);
      return;
    }

    handleHVAC(temperature, humidity);
    handleAlarms(mq2Value, pirValue);
    updateOLED(temperature, humidity, pirValue, mq2Value);
    handleNetwork(temperature, humidity, pirValue, mq2Value);
    updateAttributes(); 

    delay(500);
    if (!client.connected()) {
    reconnectMQTT();
    }
    client.loop();

    Serial.print("Reading sensors: Temp=");
    Serial.print(temperature);
    Serial.print(", Humidity=");
    Serial.print(humidity);
    Serial.print(", PIR Value=");
    Serial.print(pirValue);
    Serial.print(", MQ2 Value=");
    Serial.println(mq2Value);
  }
}
