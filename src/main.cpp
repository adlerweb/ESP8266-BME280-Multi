/***************************************************************************
  BME280 Multi Output Thingy
  Reads an BME280 using ESP8266 and provides the results via Serial/USB,
    an internal HTTP-Server, MQTT (with TLS) and HTTP-GET to a Volkszähler

  This script requires the Adafruit BME280-Library. This library is
  written by Limor Fried & Kevin Townsend for Adafruit Industries.

  This script requires the PubSubClient-Library. This library is
  written by Nicholas O'Leary

  This script is written by Florian Knodt - www.adlerweb.info
 ***************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>       

extern "C" {
  #include "user_interface.h" //os_timer
}

// WiFi Configuration
const char* cfg_wifi_ssid = "MeinWiFi";
const char* cfg_wifi_password = "geheim";

#define USE_MQTT
// MQTT Configuration
const char* mqtt_server = "meinmqtt";
const unsigned int mqtt_port = 1883;
const char* mqtt_user =   "sonoffbasic1";
const char* mqtt_pass =   "auchgeheim";
const char* mqtt_root = "/esp/bme280";

// echo | openssl s_client -connect localhost:1883 |& openssl x509 -fingerprint -noout
const char* mqtt_fprint = "aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa bb cc dd";

#define USE_VOLKSZAEHLER
const char* vz_url = "http://192.168.1.1/middleware.php/data/";
const char* vz_addcmd = ".json?operation=add&value=";

//For UUIDs: Use "" to skip this measurement
const char* vz_uuid_temp   = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee1";  //Temperature (°C)
const char* vz_uuid_dew    = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee2";  //Dew point (°C)
const char* vz_uuid_hum    = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee3";  //Absolute humidity (g/m3)
const char* vz_uuid_hum_r  = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee4";  //Relative humidity (%)
const char* vz_uuid_pres   = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee5";  //Absolute atmospheric pressure
const char* vz_uuid_pres_r = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee6";  //Relative atmospheric pressure (hPa @ sea level)

//periodic status reports
const unsigned int stats_interval = 60;  // Update statistics and measure every 60 seconds

#define ALTITUDE 123 //Altitude of your location (m above sea level)

Adafruit_BME280 bme; // I2C
ESP8266WebServer server(80);

#define CONFIG_MQTT_TOPIC_GET "/get"
#define CONFIG_MQTT_TOPIC_GET_TEMP "/temperature"
#define CONFIG_MQTT_TOPIC_GET_DEW "/dewpoint"
#define CONFIG_MQTT_TOPIC_GET_HUM "/humidity_abs"
#define CONFIG_MQTT_TOPIC_GET_HUMR "/humidity"
#define CONFIG_MQTT_TOPIC_GET_PRES "/pressure"
#define CONFIG_MQTT_TOPIC_GET_PRESR "/pressure_rel"
#define CONFIG_MQTT_TOPIC_SET "/set"
#define CONFIG_MQTT_TOPIC_SET_RESET "/reset"
#define CONFIG_MQTT_TOPIC_SET_UPDATE "/update"
#define CONFIG_MQTT_TOPIC_SET_PING "/ping"
#define CONFIG_MQTT_TOPIC_SET_PONG "/pong"
#define CONFIG_MQTT_TOPIC_STATUS "/status"
#define CONFIG_MQTT_TOPIC_STATUS_ONLINE "/online"
#define CONFIG_MQTT_TOPIC_STATUS_HARDWARE "/hardware"
#define CONFIG_MQTT_TOPIC_STATUS_VERSION "/version"
#define CONFIG_MQTT_TOPIC_STATUS_INTERVAL "/statsinterval"
#define CONFIG_MQTT_TOPIC_STATUS_IP "/ip"
#define CONFIG_MQTT_TOPIC_STATUS_MAC "/mac"
#define CONFIG_MQTT_TOPIC_STATUS_UPTIME "/uptime"
#define CONFIG_MQTT_TOPIC_STATUS_SIGNAL "/rssi"

#define MQTT_PRJ_HARDWARE "esp8266tls-bme280"
#define MQTT_PRJ_VERSION "0.0.1"

const float cToKOffset = 273.15;
float absoluteHumidity(float temperature, float humidity);
float saturationVaporPressure(float temperature);
float dewPoint(float temperature, float humidity);

#ifdef USE_MQTT
class PubSubClientWrapper : public PubSubClient{
  private:
  public:
    PubSubClientWrapper(Client& espc);
    bool publish(StringSumHelper topic, String str);
    bool publish(StringSumHelper topic, unsigned int num);
    bool publish(const char* topic, String str);
    bool publish(const char* topic, unsigned int num);

    bool publish(StringSumHelper topic, String str, bool retain);
    bool publish(StringSumHelper topic, unsigned int num, bool retain);
    bool publish(const char* topic, String str, bool retain);
    bool publish(const char* topic, unsigned int num, bool retain);
};

PubSubClientWrapper::PubSubClientWrapper(Client& espc) : PubSubClient(espc){

}

bool PubSubClientWrapper::publish(StringSumHelper topic, String str) {
  return publish(topic.c_str(), str);
}

bool PubSubClientWrapper::publish(StringSumHelper topic, unsigned int num) {
  return publish(topic.c_str(), num);
}

bool PubSubClientWrapper::publish(const char* topic, String str) {
  return publish(topic, str, false);
}

bool PubSubClientWrapper::publish(const char* topic, unsigned int num) {
  return publish(topic, num, false);
}

bool PubSubClientWrapper::publish(StringSumHelper topic, String str, bool retain) {
  return publish(topic.c_str(), str, retain);
}

bool PubSubClientWrapper::publish(StringSumHelper topic, unsigned int num, bool retain) {
  return publish(topic.c_str(), num, retain);
}

bool PubSubClientWrapper::publish(const char* topic, String str, bool retain) {
  char buf[128];

  if(str.length() >= 128) return false;

  str.toCharArray(buf, 128);
  return PubSubClient::publish(topic, buf, retain);
}

bool PubSubClientWrapper::publish(const char* topic, unsigned int num, bool retain) {
  char buf[6];

  dtostrf(num, 0, 0, buf);
  return PubSubClient::publish(topic, buf, retain);
}
#endif

os_timer_t Timer1;
bool sendStats = true;

WiFiClientSecure espClient;
#ifdef USE_MQTT
  PubSubClientWrapper client(espClient);
#endif

void timerCallback(void *arg) {
  sendStats = true;
}

uint8_t rssiToPercentage(int32_t rssi) {
  //@author Marvin Roger - https://github.com/marvinroger/homie-esp8266/blob/ad876b2cd0aaddc7bc30f1c76bfc22cd815730d9/src/Homie/Utils/Helpers.cpp#L12
  uint8_t quality;
  if (rssi <= -100) {
    quality = 0;
  } else if (rssi >= -50) {
    quality = 100;
  } else {
    quality = 2 * (rssi + 100);
  }

  return quality;
}

void ipToString(const IPAddress& ip, char * str) {
  //@author Marvin Roger - https://github.com/marvinroger/homie-esp8266/blob/ad876b2cd0aaddc7bc30f1c76bfc22cd815730d9/src/Homie/Utils/Helpers.cpp#L82
  snprintf(str, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

#ifdef USE_VOLKSZAEHLER
  bool sendVz(const char* uuid, float value) {

    if(strlen(uuid) == 0) return true;

    HTTPClient http;
    bool ret = false;
    
    String url = vz_url;
    url += uuid;
    url += vz_addcmd;
    url += (String)value;

    Serial.print(F("[vz] HTTP-Request: "));
    Serial.print(url);
    Serial.print(F(" -> "));
    http.begin(url);
    int httpCode = http.GET();

    if(httpCode > 0) {
      if(httpCode == HTTP_CODE_OK) {
        Serial.println(F("OK"));
        ret = true;
      }else{
        Serial.print(F("ERR: "));
        Serial.println(httpCode);
      }
    }else{
      Serial.print(F("ERR: "));
      Serial.println('0');
    }
    
    http.end();
    return ret;
  }
#endif

void sendStatsBoot(void) {
  #ifdef USE_MQTT
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_HARDWARE), MQTT_PRJ_HARDWARE, true);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_VERSION), MQTT_PRJ_VERSION, true);
    char buf[5];
    sprintf(buf, "%d", stats_interval);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_INTERVAL), buf, true);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_MAC), WiFi.macAddress(), true);
  #endif
}

void sendStatsInterval(void) {
  char buf[16]; //v4 only atm
  ipToString(WiFi.localIP(), buf);
  #ifdef USE_MQTT
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_IP), buf);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_UPTIME), (uint32_t)(millis()/1000));
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS_SIGNAL), rssiToPercentage(WiFi.RSSI()));
  #endif

  float temperature = bme.readTemperature();
  float humidity_r = bme.readHumidity();
  float humidity = absoluteHumidity(temperature, humidity_r);
  float pressure = bme.readPressure() / 100.0F;
  float pressure_r = bme.seaLevelForAltitude(ALTITUDE, pressure_r);
  float dew = dewPoint(temperature, humidity_r);

  Serial.print(F("T: "));
  Serial.print((String)temperature);
  Serial.print(F(" *C\nDP: "));
  Serial.print((String)dew);
  Serial.print(F(" *C\nH: "));
  Serial.print((String)humidity_r);
  Serial.print(F(" %\nAH: "));
  Serial.print((String)humidity);
  Serial.print(F(" g/m3\nRP: "));
  Serial.print((String)pressure_r);
  Serial.print(F(" hPa\nP: "));
  Serial.print((String)pressure);
  Serial.println(F(" hPa"));
  Serial.flush();

  yield();

  #ifdef USE_MQTT
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_TEMP), (String)temperature);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_DEW), (String)dew);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_HUM), (String)humidity);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_HUMR), (String)humidity_r);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_PRES), (String)pressure);
    client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_GET_PRESR), (String)pressure_r);
  #endif

  #ifdef USE_VOLKSZAEHLER
    sendVz(vz_uuid_temp, temperature);
    sendVz(vz_uuid_dew, dew);
    sendVz(vz_uuid_hum, humidity);
    sendVz(vz_uuid_hum_r, humidity_r);
    sendVz(vz_uuid_pres, pressure);
    sendVz(vz_uuid_pres_r, pressure_r);
  #endif
  
}

#ifdef USE_MQTT
  void verifyFingerprint() {
    if(client.connected() || espClient.connected()) return; //Already connected
  
    Serial.print(F("Checking TLS @ "));
    Serial.print(mqtt_server);
    Serial.print(F("..."));
  
    if (!espClient.connect(mqtt_server, mqtt_port)) {
      Serial.println(F("Connection failed. Rebooting."));
      Serial.flush();
      ESP.restart();
    }
    if (espClient.verify(mqtt_fprint, mqtt_server)) {
      Serial.print(F("Connection secure -> ."));
    } else {
      Serial.println(F("Connection insecure! Rebooting."));
      Serial.flush();
      ESP.restart();
    }
  
    espClient.stop();
  
    delay(100);
  }
  
  void reconnect() {
    // Loop until we're reconnected
    while (!client.connected()) {
      Serial.print(F("Attempting MQTT connection..."));
      verifyFingerprint();
      // Attempt to connect
      if (client.connect(WiFi.macAddress().c_str(), mqtt_user, mqtt_pass, ((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS + CONFIG_MQTT_TOPIC_STATUS_ONLINE).c_str(), 0, 1, "0")) {
        Serial.println(F("connected"));
        client.subscribe(((String)mqtt_root + CONFIG_MQTT_TOPIC_SET + "/#").c_str());
        client.publish((String)mqtt_root + CONFIG_MQTT_TOPIC_STATUS + CONFIG_MQTT_TOPIC_STATUS_ONLINE, "1");
      } else {
        Serial.print(F("failed, rc="));
        Serial.print(client.state());
        Serial.println(F(" try again in 5 seconds"));
        // Wait 5 seconds before retrying
        delay(5000);
      }
    }
  }
#endif

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print(F("Connecting to "));
  Serial.println(cfg_wifi_ssid);

  WiFi.mode(WIFI_STA); // Disable the built-in WiFi access point.
  WiFi.begin(cfg_wifi_ssid, cfg_wifi_password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  Serial.println(F("WiFi connected"));
  Serial.println(F("IP address: "));
  Serial.println(WiFi.localIP());
}

#ifdef USE_MQTT
  void callback(char* topic, byte* payload, unsigned int length) {
    Serial.print(F("Message arrived ["));
    Serial.print(topic);
    Serial.print(F("] "));
  
    char message[length + 1];
    for (unsigned int i = 0; i < length; i++) {
      message[i] = (char)payload[i];
    }
    message[length] = '\0';
    Serial.println(message);
  
    String topicStr = topic;
    String check = ((String)mqtt_root + CONFIG_MQTT_TOPIC_SET);
  
    if(topicStr.startsWith((String)check + CONFIG_MQTT_TOPIC_SET_RESET)) {
      Serial.println(F("MQTT RESET!"));
      Serial.flush();
      ESP.restart();
    }
  
    if(topicStr.startsWith((String)check + CONFIG_MQTT_TOPIC_SET_PING)) {
      Serial.println(F("PING"));
      client.publish(((String)mqtt_root + CONFIG_MQTT_TOPIC_GET + CONFIG_MQTT_TOPIC_SET_PONG), message, false);
      return;
    }
  
    if(topicStr.startsWith((String)check + CONFIG_MQTT_TOPIC_SET_UPDATE)) {
      Serial.println(F("OTA REQUESTED!"));
      Serial.flush();
      ArduinoOTA.begin();
  
      unsigned long timeout = millis() + (120*1000); // Max 2 minutes
      os_timer_disarm(&Timer1);
  
      while(true) {
        yield();
        ArduinoOTA.handle();
        if(millis() > timeout) break;
      }
  
      os_timer_arm(&Timer1, stats_interval*1000, true);
      return;
    }
  
    return;
  }
#endif

void handleRoot() {
  float temperature = bme.readTemperature();
  float humidity_r = bme.readHumidity();
  float pressure_r = bme.readPressure() / 100.0F;
  
  String out = "Temperature: ";
  out += temperature;
  out += "*C\nDew point: ";
  out += dewPoint(temperature, humidity_r);
  out += "*C\nRelative Humidity: ";
  out += humidity_r;
  out += "%\nAbsolute Humidity: ";
  out += absoluteHumidity(temperature, humidity_r);
  out += "g/m3\nRelative Pressure: ";
  out += pressure_r;
  out += "hPa\nAbsolute Pressure: ";
  out += bme.seaLevelForAltitude(ALTITUDE, pressure_r);
  out += "hPa";
  server.send(200, "text/plain", out);
}

void handleOTA() {
  server.send(200, "text/plain", "OTA START");
  ArduinoOTA.begin();
  
  unsigned long timeout = millis() + (120*1000); // Max 2 minutes
  os_timer_disarm(&Timer1);

  while(true) {
    yield();
    ArduinoOTA.handle();
    if(millis() > timeout) break;
  }

  os_timer_arm(&Timer1, stats_interval*1000, true);
  return;
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("BME280 HTTP-POST,HTTP-SRV,MQTT"));

    bool status;
    
    // default settings
    // (you can also pass in a Wire library object like &Wire2)
    status = bme.begin(0x76);
    if (!status) {
        Serial.println(F("Could not find a valid BME280 sensor, check wiring!"));
        while (1);
    }

    if (MDNS.begin(((String)WiFi.macAddress()).c_str())) {
      Serial.println(F("MDNS responder started"));
    }

    server.on("/", handleRoot);
    server.on("/ota", handleOTA);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println(F("HTTP-SRV started"));
    
    setup_wifi();
    #ifdef USE_MQTT
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
    #endif
  
    ArduinoOTA.setHostname(((String)WiFi.macAddress()).c_str());
    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";
  
      Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
      Serial.println(F("\nEnd"));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
      else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
    });
  
    #ifdef USE_MQTT
      if (!client.connected()) {
        reconnect();
      }
    #endif
  
    // Send boot info
    Serial.println(F("Announcing boot..."));
    sendStatsBoot();
  
    os_timer_setfn(&Timer1, timerCallback, (void *)0);
    os_timer_arm(&Timer1, stats_interval*1000, true);
}

void loop() { 
  #ifdef USE_MQTT
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
  #endif

  server.handleClient();

  if(sendStats) {
    sendStatsInterval();
    sendStats=false;
  }
}

// Relative to absolute humidity
// Based on https://carnotcycle.wordpress.com/2012/08/04/how-to-convert-relative-humidity-to-absolute-humidity/
float absoluteHumidity(float temperature, float humidity) {
  return (13.2471*pow(EULER,17.67*temperature/(temperature+243.5))*humidity/(cToKOffset+temperature));
}

// Calculate saturation vapor pressure
// Based on dew.js, Copyright 2011 Wolfgang Kuehn, Apache License 2.0
float saturationVaporPressure(float temperature) {
  if(temperature < 173 || temperature > 678) return -112; //Temperature out of range

  float svp = 0;
  if(temperature <= cToKOffset) {
    /**
      * -100-0°C -> Saturation vapor pressure over ice
      * ITS-90 Formulations by Bob Hardy published in 
      * "The Proceedings of the Third International 
      * Symposium on Humidity & Moisture",
      * Teddington, London, England, April 1998
      */

    svp = exp(-5.8666426e3/temperature + 2.232870244e1 + (1.39387003e-2 + (-3.4262402e-5 + (2.7040955e-8*temperature)) * temperature) * temperature + 6.7063522e-1 * log(temperature)); 
  }else{
    /**
      * 0°C-400°C -> Saturation vapor pressure over water
      * IAPWS Industrial Formulation 1997
      * for the Thermodynamic Properties of Water and Steam
      * by IAPWS (International Association for the Properties
      * of Water and Steam), Erlangen, Germany, September 1997.
      * Equation 30 in Section 8.1 "The Saturation-Pressure 
      * Equation (Basic Equation)"
      */

    const float th = temperature + -0.23855557567849 / (temperature - 0.65017534844798e3);
    const float a  = (th + 0.11670521452767e4) * th + -0.72421316703206e6;
    const float b  = (-0.17073846940092e2 * th + 0.12020824702470e5) * th + -0.32325550322333e7;
    const float c  = (0.14915108613530e2 * th + -0.48232657361591e4) * th + 0.40511340542057e6;

    svp = 2 * c / (-b + sqrt(b * b - 4 * a * c));
    svp *= svp;
    svp *= svp;
    svp *= 1e6;
  }
  
  yield();

  return svp;
}


// Calculate dew point in °C
// Based on dew.js, Copyright 2011 Wolfgang Kuehn, Apache License 2.0
float dewPoint(float temperature, float humidity)
{
  temperature += cToKOffset; //Celsius to Kelvin

  if(humidity < 0 || humidity > 100) return -111; //Invalid humidity
  if(temperature < 173 || temperature > 678) return -112; //Temperature out of range

  humidity = humidity / 100 * saturationVaporPressure(temperature);
  
  byte mc = 10;

  float xNew;
  float dx;
  float z;

  do {
    dx = temperature / 1000;
    z = saturationVaporPressure(temperature);
    xNew = temperature + dx * (humidity - z) / (saturationVaporPressure(temperature + dx) - z);
    if (abs((xNew - temperature) / xNew) < 0.0001) {
        return xNew - cToKOffset;
    }
    temperature = xNew;
    mc--;
  } while(mc > 0);

  return -113; //Solver did not get a close result
}
