# ESP32-C6 + BME680 + BSEC Environmental Monitor

Environmental monitoring and secure MQTT telemetry using **ESP32-C6**, **Bosch BME680**, **Bosch BSEC**, and **Zephyr RTOS**.

This project demonstrates a complete environmental IoT data path: measurements from a BME680 sensor are processed using Bosch BSEC, calibration state is persisted in non-volatile memory, and telemetry is published over Wi-Fi to an MQTT broker using TLS.

The project was developed as an environmental IoT demonstration for an IoT workshop associated with the **Around Europe Festival 2026 in Pombal, Portugal**.

Live sensor data and additional information about the IoT demonstration are available at:

**https://www.pombal-festival.eu/iot/**

---

## Features

* ESP32-C6 running Zephyr RTOS
* BME680 environmental sensor over I2C
* Bosch BSEC air-quality processing
* Indoor Air Quality (IAQ)
* IAQ accuracy status
* CO₂ equivalent estimation
* Breath VOC equivalent estimation
* Temperature, humidity, and pressure measurements
* BSEC state persistence using Zephyr Settings/NVS
* Automatic Wi-Fi connection and reconnection
* MQTT client running in a dedicated manager thread
* MQTT over TLS 1.2
* Server certificate verification
* MQTT QoS support
* JSON telemetry publishing

---

## Hardware

### ESP32-C6 DevKitC

Target board:

```text
esp32c6_devkitc/esp32c6/hpcore
```

### BME680 wiring

| BME680    | ESP32-C6 DevKitC |
| --------- | ---------------- |
| VCC / VIN | 3V3              |
| GND       | GND              |
| SDA       | GPIO6            |
| SCL       | GPIO7            |

The BME680 is configured at I2C address:

```text
0x77
```

---

## Software

The complete local project uses:

* Zephyr RTOS
* Bosch BME68x Sensor API
* Bosch BSEC
* Zephyr Wi-Fi management
* Zephyr MQTT library
* Zephyr TLS credentials
* Zephyr Settings/NVS

The public repository contains the application orchestration, Wi-Fi, MQTT/TLS, board configuration, and related project configuration files.

The sensor/BSEC integration layer and sensor/BSEC dependencies are intentionally not distributed in this repository.

---

## Architecture

The complete application is organized into several functional layers:

* `main.c` – application initialization, sensor telemetry formatting and publishing
* Sensor/BSEC integration layer – BME680 communication and Bosch BSEC processing
* BSEC storage layer – BSEC state persistence using Zephyr Settings/NVS
* `wifi_manager.c` – asynchronous Wi-Fi connection and automatic reconnection
* `mqtt_manager.c` – MQTT connection management, TLS, publishing, subscriptions and reconnect handling

The main data flow is:

```text
BME680
   |
   v
Bosch BSEC
   |
   v
Environmental data
   |
   +--------> Serial output
   |
   v
JSON telemetry
   |
   v
Wi-Fi
   |
   v
MQTT over TLS
   |
   v
MQTT broker
   |
   v
Remote telemetry / Web application
```

---

## Public repository structure

The public repository contains:

```text
esp32c6-bme680-bsec-mqtt/
├── CMakeLists.txt
├── prj.conf
├── boards/
│   └── esp32c6_devkitc_esp32c6_hpcore.overlay
├── src/
│   ├── main.c
│   ├── wifi_manager.c
│   ├── wifi_manager.h
│   ├── mqtt_manager.c
│   ├── mqtt_manager.h
│   └── ca_cert.h
├── .gitignore
├── LICENSE
└── README.md
```

### Components not included

The following parts of the complete local project are intentionally **not distributed in this repository**:

```text
src/
├── sensor_bme680.c
├── sensor_bme680.h
├── bsec_storage.c
└── bsec_storage.h

external/
├── bme68x/
│   ├── bme68x.c
│   ├── bme68x.h
│   └── bme68x_defs.h
│
└── bsec/
    ├── bsec_interface.h
    ├── bsec_datatypes.h
    ├── bsec_iaq.c
    ├── bsec_iaq.h
    └── libalgobsec.a
```

These files are used by the complete local demonstration but are intentionally excluded from the public source release.

The Bosch BSEC components must be obtained separately from Bosch Sensortec and used according to the applicable Bosch license terms.

The Bosch BME68x Sensor API and other third-party components remain subject to their respective licenses.

---

## Bosch BSEC dependency

The complete application uses **Bosch BSEC** for air-quality processing.

Bosch BSEC is **not distributed with this repository**.

Users wishing to reproduce the complete sensor implementation must obtain the appropriate BSEC package separately from Bosch Sensortec and comply with the applicable Bosch license terms.

The local project uses a precompiled BSEC library compatible with the **ESP32-C6 RISC-V target**.

---

## BME68x Sensor API

The complete local application also uses the Bosch BME68x Sensor API for communication with the BME680 sensor.

The BME68x Sensor API source files used by the local project are **not included in this public repository**.

Users wishing to reproduce the complete sensor implementation must obtain the required sensor API separately and comply with its applicable license terms.

---

## Configuration

### Wi-Fi

Configure your Wi-Fi credentials in `wifi_manager.c`:

```c
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
```

Only placeholder credentials should be committed to a public repository.

### MQTT

Configure the MQTT topic and device identifier in `main.c`:

```c
#define MQTT_SENSOR_TOPIC "your/mqtt/topic"
#define DEVICE_ID         "pombal-esp32-01"
```

Example TLS MQTT configuration:

```c
static const mqtt_manager_config_t my_mqtt_config = {
    .broker_address = "your-broker.example.com",
    .broker_port = 8883,
    .transport = MQTT_MANAGER_TRANSPORT_TLS,
    .client_id = DEVICE_ID,
    .username = "YOUR_MQTT_USERNAME",
    .password = "YOUR_MQTT_PASSWORD",
    .subscribe_topic = "your/mqtt/subscribe/topic",
    .keepalive = 60,
    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
    .tls_sec_tag = TLS_SEC_TAG_ID
};
```

Do not commit real Wi-Fi or MQTT credentials to a public repository.

---

## TLS

The application supports MQTT over TLS 1.2.

The CA certificate is registered using Zephyr's TLS credential subsystem, and the connection uses:

```text
TLS_PEER_VERIFY_REQUIRED
```

The MQTT broker hostname is provided to the TLS stack for hostname verification and SNI.

The trusted CA certificate used by the application is stored in:

```text
src/ca_cert.h
```

When using a different MQTT broker, replace or update the CA certificate if its certificate chain requires a different trusted CA.

Public CA certificates are not secret credentials. Private certificates and private keys must never be committed to the repository.

---

## MQTT telemetry

Sensor measurements are formatted as JSON before being published to the MQTT broker.

Example payload:

```json
{
  "device": "pombal-esp32-01",
  "temperature_c": 24.21149,
  "humidity_pct": 55.54113,
  "pressure_kpa": 100.5488,
  "iaq": 50.00,
  "iaq_accuracy": 3,
  "co2_equivalent_ppm": 500.00,
  "bvoc_equivalent_ppm": 0.500
}
```

The exact values depend on the sensor measurements and BSEC processing.

---

## BSEC state persistence

The complete local application stores BSEC calibration state using the Zephyr Settings subsystem backed by NVS.

This allows previously learned BSEC state to be restored after a reboot instead of starting the learning process from scratch every time.

The BSEC storage implementation used by the complete demo is intentionally not included in this public repository.

---

## Building

### Important

This repository is a **partial public source release of the demonstration project**.

The sensor/BSEC integration layer and its dependencies are intentionally not included. Therefore, a fresh clone of this repository does **not build the complete environmental monitoring application without the missing sensor/BSEC components**.

With the required local components in place, the project is built for the ESP32-C6 DevKitC using:

```bash
west build -b esp32c6_devkitc/esp32c6/hpcore
```

To create a clean build:

```bash
west build -p always -b esp32c6_devkitc/esp32c6/hpcore
```

Flash the ESP32-C6 with:

```bash
west flash
```

Monitor the serial output using your preferred serial terminal.

---

## BSEC IAQ accuracy

After a fresh start, BSEC IAQ accuracy may initially be `0`.

The BSEC IAQ accuracy value ranges from `0` to `3`:

```text
0 -> Stabilizing
1 -> Low accuracy
2 -> Medium accuracy
3 -> High accuracy
```

For meaningful IAQ measurements and comparisons, allow the sensor and BSEC algorithm sufficient time to stabilize and learn the operating environment.

Persisting the BSEC state allows previously learned calibration information to be restored after a restart.

---

## Security

This public repository is intended to contain placeholder credentials only.

Before committing or publishing changes, verify that the following are not included:

* Wi-Fi passwords
* Private Wi-Fi configuration
* MQTT usernames and passwords
* Private MQTT broker credentials
* API keys or access tokens
* Private certificates
* Private keys
* Other authentication credentials

Public CA certificates are not secret credentials.

---

## Project context

This project was developed as an environmental IoT demonstration for an IoT workshop associated with the **Around Europe Festival 2026 in Pombal, Portugal**.

The goal of the demonstration is to show a complete embedded IoT data path using an ESP32-C6:

```text
Environmental sensing
        |
        v
BSEC air-quality processing
        |
        v
Persistent BSEC state
        |
        v
Wi-Fi
        |
        v
MQTT over TLS
        |
        v
MQTT broker
        |
        v
Remote telemetry / Web application
```

Live sensor data and information about the demonstration:

**https://www.pombal-festival.eu/iot/**

---

## License and third-party components

Project-specific source code distributed in this repository is licensed under the **BSD 3-Clause License**. See the `LICENSE` file for details.

Third-party components remain subject to their respective licenses.

The sensor/BSEC integration source files used by the complete local demonstration are intentionally not included in this public repository.

The Bosch BME68x Sensor API used by the complete application is not included in this repository and remains subject to its applicable license terms.

Bosch BSEC is a separate dependency and is **not included in this repository**. Users wishing to reproduce the complete application must obtain BSEC separately from Bosch Sensortec and comply with the applicable Bosch license terms.

The BSD 3-Clause License included with this repository does not replace, override, or modify the license terms of third-party components.

---

## Acknowledgements

This project uses:

* Zephyr RTOS
* Bosch BME680
* Bosch BME68x Sensor API
* Bosch BSEC

Thanks to the organizations and developers behind these technologies and tools.

---

## Author

**Dejan**

GitHub: **Dejan-R**

Email: **[dejan.rakijasic@gmail.com](mailto:dejan.rakijasic@gmail.com)**

Project developed in Croatia for the Around Europe Festival 2026 IoT workshop in Pombal, Portugal.

---

## Demo

Live IoT demonstration:

**https://www.pombal-festival.eu/iot/**
