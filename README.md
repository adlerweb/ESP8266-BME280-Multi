# ESP8266-BME280-Multi
ESP8266 + BME280 -> MQTT, Serial, HTTP-Server, HTTP-Client/Volkszähler

Reads an BME280 using ESP8266 and provides the results via Serial/USB, an internal HTTP-Server, MQTT (with TLS) and HTTP-GET to a [Volkszähler](http://volkszaehler.org/)

# Wiring
| BME280 | ESP8266              |
|--------|----------------------|
| VCC    | 3.3V                 |
| GND    | GND                  |
| SDA    | GPIO4 (D2 @ NodeMCU) |
| SCL    | GPIO5 (D1 @ NodeMCU) |

# Configuration
Insert your WiFi-Parameters in the main code (src/main.cpp). If you use MQTT add the corresponding parameters, otherwise remove USE_MQTT. Same goes for the Volkszähler-Settings (VZ_) and USE_VOLKSZAEHLER. You can control every how many seconds the sensor is polled and the values distributed using ***stats_interval***.

# Building

When using [PlatformIO](https://www.platformio.org) just download/clone and open the project folder. You should be able to build everything right away.
On Arduino ensure [PubSubClient](https://github.com/knolleary/pubsubclient) is installed.

# Functionality

## Serial

Every ***stats_interval*** seconds the following text will appear:

```
T: 12.34 *C
DP: 12.34 *C
H: 12.34 %
AH: 1.34 g/m3
RP: 1234.56 hPa
P: 1234.56 hPa
```

T is temperature, DP dew pont, H relative humidity, AH absolute humidity, RP relative pressure and P absolute pressure at sea level.

If you're using Volkszähler additional debug output with the computed URLs and HTTP-Status will be present.

## HTTP-Server

Just access the module using HTTP. You can get the modules IP using serial, MQTT or your routers DHCP-leases. You'll find all values mentioned above. Every access triggers the module to gather a new set of values from the sensor, so the display should always display up to date measurements.

Access /ota to start an OTA-update.

## MQTT

This code assumes you use TLS and password-authentication. All path originate from ***mqtt_root***. On boot the module will publish static parameters:

```
/esp/bme280/status/online 1
/esp/bme280/hardware esp8266tls-bme280
/esp/bme280/version 0.0.1
/esp/bme280/statsinterval 60
/esp/bme280/mac 11:22:33:44:55:66
```

Statsinterval is in seconds.

Every ***stats_interval*** seconds variable data will be published:

/esp/bme280/ip 192.168.123.123
/esp/bme280/uptime 123
/esp/bme280/rssi 100
/esp/bme280/get/temperature 12.34
/esp/bme280/get/dewpoint 12.34
/esp/bme280/get/humidity_abs 12.34
/esp/bme280/get/humidity 12.34
/esp/bme280/get/pressure 1234.56
/esp/bme280/get/pressure_rel 1234.56

Uptime is in seconds, RSSI in %.

Additional commands are:
* /esp/bme280/set/ping -> Received value is requblished to /esp/bme280/get/pong
* /esp/bme280/set/reset -> Device reboots
* /esp/bme280/set/update -> Device enters ArduinoOTA mode

## Volkszähler

Add new channels for the corresponding measurements using the frontend. Note the UUIDs and add them to the corresponding configuration values. Check ***vz_url*** points to your middleware.php/data. If you do not want to record a specific measurement set the UUID to "".
