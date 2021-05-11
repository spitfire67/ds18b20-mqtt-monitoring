// Include the libraries we need
#include <OneWire.h>
#include <DallasTemperature.h>

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiGeneric.h>

#include <PubSubClient.h>

#include <EEPROM.h>

typedef struct
{
  const char *name;
  const char *displayName;
  DeviceAddress device;
} DeviceInfo;

// GPIO where the data wire is plugged (multiple DS18B20 devices can be on the same data wire)
// BUS 2 = Pin D4
#define ONE_WIRE_BUS 2
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"
#define MQTT_SERVER "192.168.0.2"
#define MQTT_PORT 1883
#define MQTT_USER "user"
#define MQTT_PASSWORD "password"
#define MQTT_CLIENT_ID "ESP8266-D1mini"
#define MQTT_TOPIC "PoolTemperature"

// Text
#define WEB_TITLE Pool - Monitoring
#define WEB_BUTTON_REFRESH "Refresh"
#define WEB_BUTTON_BACK "Back"
#define WEB_BUTTON_SAVE "Save"
#define WEB_LABEL_DELAY_MESSAGE "Delay between 2 MQTT measurements (in seconds)"
#define WEB_LABEL_CHANGE_SAVED_MESSAGE "The changes have been saved"

// Arrays to hold device address
#define DEVICE_NUMBER 2

DeviceInfo devices[] = {{"Water1", "Water", {0x28, 0x2F, 0xF1, 0x6D, 0x32, 0x20, 0x01, 0xE4}},
                        {"Air1", "Air", {0x28, 0x2D, 0xDD, 0x64, 0x32, 0x20, 0x01, 0xC0}}};

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);

// Local IP address of the current device once connected to a network
String localIP;

// Wifi Handlers
WiFiEventHandler e1, e2;

// Web server
ESP8266WebServer webServer(80);

// MQTT client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Timer variables
unsigned long deviceLastTime = 0;
unsigned long mqttLastTime = 0;

// Represents the delay between 2 measurements (configurable)
unsigned long deviceTimerDelay;
// Represents the delay between 2 tries when MQTT is disconnected.
unsigned long mqttTimerDelay = 5000;

// Address where to store the delay value
const int delayValueAddress = 0;
// The delay value with the default value
unsigned int delayValueInSeconds = 5;

// Write an unsigned int to EEPROM
void writeUnsignedIntIntoEEPROM(int address, unsigned int number)
{
  EEPROM.write(address, number >> 8);
  EEPROM.write(address + 1, number & 0xFF);
  EEPROM.commit();
}

// Read an unsigned int to EEPROM
unsigned int readUnsignedIntFromEEPROM(int address)
{
  return (EEPROM.read(address) << 8) + EEPROM.read(address + 1);
}

void setupEEPROM()
{
  // Initialize the EEPROM with a size in bytes.
  // Size of one unsigned int = 2 bytes.
  EEPROM.begin(2);
}

void setupWifi()
{
  e1 = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &event) {
    Serial.println("");
    Serial.println("WiFi connected !");
    Serial.print("IP address: ");
    localIP = WiFi.localIP().toString();
    Serial.println(localIP);
  });

  e2 = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &event) {
    // Note: this event is raised every second while we are disconnected.
    // Once wifi is back the device is connected again.
    Serial.println("Disconnected from Wi-Fi.");
  });

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  //connect to your local wi-fi network
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  //check wi-fi is connected to wi-fi network
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
}

void setupMqtt()
{
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
}

void tryReconnectMqtt()
{
  String clientId = MQTT_CLIENT_ID;
  Serial.print("Attempting MQTT connection...");
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD))
  {
    Serial.println("connected");
  }
  else
  {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
  }
}

String getTemperature(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  if (tempC == DEVICE_DISCONNECTED_C)
  {
    return "--";
  }

  return String(tempC);
}

// function to print the temperature for a device
void printTemperature(DeviceAddress deviceAddress)
{
  String tempC = getTemperature(deviceAddress);
  if (tempC == "--")
  {
    Serial.println("Error: Could not read temperature data");
    return;
  }
  Serial.print("Temp C: ");
  Serial.println(tempC);
}

void getAndPublishTemperatures()
{
  // request to all devices on the bus
  Serial.print("Requesting temperatures...");
  sensors.requestTemperatures();
  Serial.println("DONE");

  String json = "[";
  for (int i = 0; i < DEVICE_NUMBER; i++)
  {
    String tempC = getTemperature(&devices->device[i]);
    String deviceName = String(devices[i].name);

    if (i != 0)
    {
      json += ",";
    }
    json += "{\"device\":\"" + deviceName + "\", \"ip\":\"" + localIP + "\", \"sensorType\": \"Temperature\",\"value\":\"" + String(tempC) + "\"}";
  }

  json += "]";
  const char *payload = json.c_str();
  Serial.println(payload);
  if (json != "[]")
  {
    Serial.println("push message");
    mqttClient.publish(MQTT_TOPIC, payload, true);
  }
}

void loop(void)
{
  webServer.handleClient();

  unsigned long currentMillis = millis();
  if ((currentMillis - deviceLastTime) > deviceTimerDelay && mqttClient.connected())
  {
    getAndPublishTemperatures();
    deviceLastTime = currentMillis;
  }

  if (!mqttClient.connected() && (currentMillis - mqttLastTime) > mqttTimerDelay)
  {
    tryReconnectMqtt();
    mqttLastTime = currentMillis;
  }

  // This should be called regularly to allow the client to process incoming messages and maintain its connection to the server.
  mqttClient.loop();
}

void handle_OnUpdate()
{
  if (webServer.hasArg("delay"))
  {
    String stringValue = webServer.arg("delay");
    unsigned int value = atoi(stringValue.c_str());
    setTimeDelay(value, true);
  }
  else
  {
    Serial.println("Unable to get value");
  }

  webServer.send(200, "text/html", sendUpdateHtml());
}

void handle_OnTemperature()
{
  // request to all devices on the bus
  Serial.print("Requesting temperatures...");
  sensors.requestTemperatures();
  Serial.println("DONE");

  String json = "[";
  for (int i = 0; i < DEVICE_NUMBER; i++)
  {
    String tempC = getTemperature(&devices->device[i]);
    String deviceName = String(devices[i].name);

    if (i != 0)
    {
      json += ",";
    }
    json += "{\"device\":\"" + deviceName + "\", \"value\":\"" + String(tempC) + "\"}";
  }

  json += "]";
  const char *payload = json.c_str();
  webServer.send(200, "application/json", payload);
  Serial.println(payload);
}

void handle_OnMqtt()
{
  getAndPublishTemperatures();
  webServer.send(200, "text/html", "OK");
}

void handle_OnConnect()
{
  webServer.send(200, "text/html", SendHTMLNew(localIP, String(delayValueInSeconds)));
}

void handle_NotFound()
{
  webServer.send(404, "text/plain", "Not found");
}

void handle_OnSerial()
{
  // request to all devices on the bus
  Serial.print("Requesting temperatures...");
  sensors.requestTemperatures();
  Serial.println("DONE");

  for (int i = 0; i < DEVICE_NUMBER; i++)
  {
    Serial.print("Display for device ");
    Serial.println(i);
    printTemperature(&devices->device[i]);
  }

  webServer.send(200, "text/html", "OK");
}

// function to print a device address
void printAddress(DeviceAddress deviceAddress)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    if (deviceAddress[i] < 16)
    {
      Serial.print("0");
    }
    Serial.print(deviceAddress[i], HEX);
  }
}

String SendHTMLNew(String ip, String delay)
{
  String ptr = "<!DOCTYPE html><html>\n";
  ptr += "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
  ptr += "<title>WEB_TITLE</title>\n";
  ptr += "<style>html{font-family: Helvetica, sans-serif;display: inline-block;margin: 0px auto;text-align: center;}\n";
  ptr += "body{margin-top: 50px;color: #444444;}h1{margin: 50px auto 30px;}button{margin-top: 1em;padding: 0.25em 1.5em;}\n";
  ptr += "p{font-size: 24px;margin-bottom: 10px;} #settings{margin: 2em;padding: 2em;border-top: 1px black solid;}</style></head>\n";
  ptr += "<body>\n";
  ptr += "<div><h1>WEB_TITLE</h1><div id=\"content\"></div>\n";
  ptr += "<button onclick=\"window.location.reload()\">WEB_BUTTON_REFRESH</button></div>\n";
  ptr += "<div id=\"settings\"><form action=\"update\">\n";
  ptr += "<label for=\"delay\">WEB_LABEL_DELAY_MESSAGE:</label>\n";
  ptr += "<input id=\"delay\" name=\"delay\" type=\"number\" min=\"1\" max=\"9999\" size=\"4\" pattern=\"[0-9]+\" value=\"" + delay + "\" required /><br />\n";
  ptr += "<button type=\"submit\">WEB_BUTTON_SAVE</button></form>\n";
  ptr += "<script>\n";
  ptr += "function update() {\n";
  ptr += "var xhttp = new XMLHttpRequest();\n";
  ptr += "xhttp.onreadystatechange = function () {\n";
  ptr += "if (this.readyState == 4 && this.status == 200) { display(JSON.parse(this.responseText)); } };\n";
  ptr += "xhttp.open(\"GET\", \"http://" + ip + "/temperature\", true); xhttp.send();\n";
  ptr += "};\n";
  ptr += "function display(payload) {\n";
  ptr += "var out = \"\"; for(var i = 0; i < payload.length; i++) {\n";
  ptr += "out += '<p>' +  payload[i].device + ': ' + payload[i].value + ' &deg;C</p>';\n";
  ptr += "}\n";
  ptr += "document.getElementById(\"content\").innerHTML = out;\n";
  ptr += "}\n";
  ptr += "update(); setInterval(update, 10000);\n";
  ptr += "</script></div></body></html>\n";
  return ptr;
}

String sendUpdateHtml()
{
  String ptr = "<!DOCTYPE html><html>\n";
  ptr += "<head><meta name=\"viewport\" content=\"width=device-width\">\n";
  ptr += "<title>WEB_TITLE</title>\n";
  ptr += "<style>html { font-family: Helvetica, sans-serif; display: inline-block; margin: 5px auto; text-align: center;}\n";
  ptr += "body{margin-top: 2em;color: #444444;}button{margin-top: 1em;}</style>\n";
  ptr += "</head>\n";
  ptr += "<body><div><span>WEB_LABEL_CHANGE_SAVED_MESSAGE.</span></div>\n";
  ptr += "<div><button onclick=\"window.location.href = '/';\">WEB_BUTTON_BACK</button></div>\n";
  ptr += "</div></body></html>\n";
  return ptr;
}

void setupWebServer()
{
  webServer.on("/", handle_OnConnect);
  webServer.on("/mqtt", handle_OnMqtt);
  webServer.on("/serial", handle_OnSerial);
  webServer.on("/update", handle_OnUpdate);
  webServer.on("/temperature", handle_OnTemperature);
  webServer.onNotFound(handle_NotFound);

  // Start the server
  webServer.begin();
  Serial.println("HTTP server started");
  Serial.println("http://" + localIP);
}

void setTimeDelay(unsigned int value, bool saveToEEPROM)
{
  if (value <= 0 || value > 9999)
  {
    Serial.println("The value is invalid");
  }
  else
  {
    delayValueInSeconds = value;
    deviceTimerDelay = delayValueInSeconds * 1000;

    if (saveToEEPROM)
    {
      Serial.println("Save value to EEPROM");
      writeUnsignedIntIntoEEPROM(delayValueAddress, delayValueInSeconds);
    }
  }

  Serial.print("The delay value is (in seconds): ");
  Serial.println(delayValueInSeconds);
}

// Main setup function
void setup(void)
{
  // start serial port
  Serial.begin(9600);
  delay(100);

  Serial.println("Dallas Temperature Monitoring");

  // locate devices on the bus
  Serial.print("Locating devices...");
  sensors.begin();
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount(), DEC);
  Serial.println(" devices.");

  // report parasite power requirements
  Serial.print("Parasite power is: ");
  if (sensors.isParasitePowerMode())
  {
    Serial.println("ON");
  }
  else
  {
    Serial.println("OFF");
  }

  for (byte i = 0; i < DEVICE_NUMBER; i++)
  {
    if (!sensors.getAddress(devices[i].device, i))
    {
      Serial.print("Unable to find at the specificed address the device ");
      Serial.println(i);
      continue;
    }

    Serial.print("Address for device ");
    Serial.print(i);
    Serial.print(": ");
    printAddress(devices[i].device);
    Serial.println();

    // set the resolution to 9 bit (Each Dallas/Maxim device is capable of several different resolutions)
    sensors.setResolution(&devices->device[i], 9);

    Serial.print("Device Resolution: ");
    Serial.print(sensors.getResolution(&devices->device[i]), DEC);
    Serial.println();
  }

  // Setup Wifi
  setupWifi();

  // Setup of the web server
  setupWebServer();

  // Setup MQTT
  setupMqtt();

  // Setup EEPROM
  setupEEPROM();

  // Setup user preferences
  unsigned int value = readUnsignedIntFromEEPROM(delayValueAddress);
  setTimeDelay(value, false);
}
