
 /*
 * Pombal IoT Workshop Demo
 *
 * ESP32-C6 + BME680 + Bosch BSEC + Zephyr RTOS
 *
 * This application demonstrates an IoT environmental monitoring node based on the ESP32-C6 and Bosch BME680 sensor.
 *
 * Features:
 * - BME680 environmental sensing over I2C
 * - Bosch BSEC air-quality processing
 * - BSEC state persistence using Zephyr Settings/NVS
 * - Wi-Fi connectivity with automatic reconnection
 * - MQTT communication over TLS
 * - JSON telemetry publishing
 *
 * Hardware:
 *
 *   BME680          ESP32-C6 DevKitC
 *   VCC/VIN   --->  3V3
 *   GND       --->  GND
 *   SDA       --->  GPIO6
 *   SCL       --->  GPIO7
 *
 *
 * Bosch BSEC dependency:
 * Bosch BSEC is required to build this project but is not distributed with the project. 
 * It must be obtained separately from Bosch Sensortec and used according to the applicable Bosch license terms.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/net/tls_credentials.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "sensor_bme680.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ca_cert.h"


 // MQTT CONFIGURATION
#define USE_SECURE_MQTT 1
#define TLS_SEC_TAG_ID  1

#define MQTT_SENSOR_TOPIC "your/mqtt/topic"
#define DEVICE_ID         "pombal-esp32-01"

#if USE_SECURE_MQTT

static const mqtt_manager_config_t my_mqtt_config = {
    .broker_address = "your-broker.example.com",
    .broker_port = 8883,
    .transport = MQTT_MANAGER_TRANSPORT_TLS,
    .client_id = "pombal-esp32-01",
    .username = "YOUR_MQTT_USERNAME",
    .password = "YOUR_MQTT_PASSWORD",
    .subscribe_topic = "your/mqtt/subscribe/topic",
    .keepalive = 60,
    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
    .tls_sec_tag = TLS_SEC_TAG_ID
};

#else

static const mqtt_manager_config_t my_mqtt_config = {
    .broker_address = "YOUR_BROKER_IP",
    .broker_port = 1883,
    .transport = MQTT_MANAGER_TRANSPORT_TCP,
    .client_id = "pombal-esp32-01",
    .username = "YOUR_MQTT_USERNAME",
    .password = "YOUR_MQTT_PASSWORD",
    .subscribe_topic = "your/mqtt/subscribe/topic",
    .keepalive = 60,
    .qos = MQTT_QOS_0_AT_MOST_ONCE,
    .tls_sec_tag = -1
};

#endif


/* 
 * FLOAT -> STRING
*/
static int float_to_string(
    char *buffer,
    size_t buffer_size,
    float value,
    uint8_t decimals)
{
    int32_t multiplier = 1;
    for (uint8_t i = 0;
         i < decimals;
         i++) {
        multiplier *= 10;
    }

    float scaled_f =value * (float)multiplier;
    int32_t scaled;
    if (scaled_f >= 0.0f) {
        scaled =(int32_t)(scaled_f + 0.5f);
    }
    else {
        scaled =(int32_t)(scaled_f - 0.5f);
    }
    bool negative = (scaled < 0);
    if (negative) {
         scaled =-scaled;
    }

    int32_t whole =  scaled / multiplier;
    int32_t fraction =  scaled % multiplier;

    if (decimals == 0) {
        return snprintk(
            buffer,
            buffer_size,
            "%s%d",
            negative ? "-" : "",
            whole
        );
    }
    return snprintk(
        buffer,
        buffer_size,
        "%s%d.%0*d",
        negative ? "-" : "",
        whole,
        decimals,
        fraction
    );
}


/* ============================================================
 * CREATE SENSOR JSON
 *
 * Format:
 *
 * {
 *   "device": "pombal-esp32-01",
 *   "temperature_c": 24.21149,
 *   "humidity_pct": 55.54113,
 *   "pressure_kpa": 100.5488,
 *   "iaq": 50,
 *   "iaq_accuracy": 0,
 *   "co2_equivalent_ppm": 500,
 *   "bvoc_equivalent_ppm": 0.5
 * }
 * ============================================================
 */

static int create_sensor_json(
    char *json,
    size_t json_size,
    const struct sensor_bme680_data *data)
{
    char temperature[24];
    char humidity[24];
    char pressure[24];
    char iaq[24];
    char co2[24];
    char bvoc[24];
    float_to_string(
        temperature,
        sizeof(temperature),
        data->temperature,
        5
    );

    float_to_string(
        humidity,
        sizeof(humidity),
        data->humidity,
        5
    );

    float_to_string(
        pressure,
        sizeof(pressure),
        data->pressure,
        4
    );

    float_to_string(
        iaq,
        sizeof(iaq),
        data->iaq,
        2
    );

    float_to_string(
        co2,
        sizeof(co2),
        data->co2_equivalent,
        2
    );

    float_to_string(
        bvoc,
        sizeof(bvoc),
        data->breath_voc_equivalent,
        3
    );

    int len = snprintk(
        json,
        json_size,

        "{"
        "\"device\":\"%s\","
        "\"temperature_c\":%s,"
        "\"humidity_pct\":%s,"
        "\"pressure_kpa\":%s,"
        "\"iaq\":%s,"
        "\"iaq_accuracy\":%u,"
        "\"co2_equivalent_ppm\":%s,"
        "\"bvoc_equivalent_ppm\":%s"
        "}",

        DEVICE_ID,
        temperature,
        humidity,
        pressure,
        iaq,
        data->iaq_accuracy,
        co2,
        bvoc
    );


    if ((len < 0) ||
        ((size_t)len >= json_size)) {

        return -ENOMEM;
    }

    return len;
}


 /* PRINT SENSOR DATA */
static void print_sensor_data(
    const struct sensor_bme680_data *data)
{
    char value[24];
    printk("\n");
    printk(
        "-------- BME680 / BSEC --------\n"
    );
    float_to_string(
        value,
        sizeof(value),
        data->temperature,
        2
    );

    printk(
        "Temperature comp: %s C\n",
        value
    );

    float_to_string(
        value,
        sizeof(value),
        data->pressure,
        3
    );

    printk(
        "Pressure        : %s kPa\n",
        value
    );


    float_to_string(
        value,
        sizeof(value),
        data->humidity,
        2
    );

    printk(
        "Humidity comp   : %s %%\n",
        value
    );

    float_to_string(
        value,
        sizeof(value),
        data->gas_resistance,
        0
    );

    printk(
        "Gas resistance  : %s ohm\n",
        value
    );

    printk("\n");

    float_to_string(
        value,
        sizeof(value),
        data->iaq,
        2
    );

    printk(
        "IAQ             : %s\n",
        value
    );

    printk(
        "IAQ accuracy    : %u (%s)\n",
        data->iaq_accuracy,
        sensor_bme680_iaq_status_text()
    );

    float_to_string(
        value,
        sizeof(value),
        data->co2_equivalent,
        2
    );

    printk(
        "CO2 equivalent  : %s ppm\n",
        value
    );


    float_to_string(
        value,
        sizeof(value),
        data->breath_voc_equivalent,
        3
    );

    printk(
        "Breath VOC eq   : %s ppm\n",
        value
    );
    printk(
        "--------------------------------\n"
    );
}



int main(void)
{
    int ret;

    printk("\n");
    printk(
        "========================================\n"
    );
    printk(
        " ESP32-C6 + BME680 + BSEC + WiFi + MQTT\n"
    );
    printk(
        "========================================\n"
    );


     /* 1. BME680 + BSEC */
    ret = sensor_bme680_init();
    if (ret != 0) {
        printk(
            "[APP] BME680/BSEC initialization FAILED: %d\n",
            ret
        );
        return ret;
    }


    printk(
        "[APP] BME680/BSEC READY\n"
    );

       /* 2. WIFI */
    printk(
        "[APP] Starting WiFi manager...\n"
    );
    wifi_manager_init();


       /* 3. TLS CA CERTIFICATE */
    if (my_mqtt_config.transport ==
        MQTT_MANAGER_TRANSPORT_TLS) {
        ret =
            tls_credential_add(
                TLS_SEC_TAG_ID,
                TLS_CREDENTIAL_CA_CERTIFICATE,
                ca_certificate,
                sizeof(ca_certificate)
            );

        if ((ret < 0) &&
            (ret != -EALREADY)) {

            printk(
                "[APP] Failed to add CA certificate: %d\n",
                ret
            );
            return ret;
        }
        printk(
            "[APP] CA certificate registered on tag %d\n",
            TLS_SEC_TAG_ID
        );
    }



  /* 4. MQTT MANAGER */
    ret =
        mqtt_manager_init(
            &my_mqtt_config
        );

    if (ret != 0) {
        printk(
            "[APP] MQTT manager initialization FAILED: %d\n",
            ret
        );
        return ret;
    }
    printk(
        "[APP] MQTT manager started\n"
    );
    printk("\n");
    printk(
        "[APP] Application running...\n"
    );
    printk("\n");

  
     /* JSON BUFFER */
    char json[512];


    while (1) {
         /* BME680 / BSEC UPDATE */
        ret =sensor_bme680_update();
        if (ret < 0) {
            printk(
                "[APP] sensor_bme680_update ERROR: %d\n",
                ret
            );
            k_sleep(
                K_MSEC(500)
            );
            continue;
        }

         /* NEW BSEC DATA */
        if (ret > 0) {
            const struct sensor_bme680_data *data =
                sensor_bme680_get_data();
    
             /* LOCAL SERIAL OUTPUT */
            print_sensor_data(
                data
            );

            /* MQTT */
            if (mqtt_manager_is_ready()) {

                int json_len =
                    create_sensor_json(
                        json,
                        sizeof(json),
                        data
                    );

                if (json_len < 0) {

                    printk(
                        "[APP] JSON creation FAILED: %d\n",
                        json_len
                    );
                }
                else {

                    printk(
                        "[APP] MQTT TX topic: %s\n",
                        MQTT_SENSOR_TOPIC
                    );


                    printk(
                        "[APP] MQTT TX payload: %s\n",
                        json
                    );
                    ret =
                        mqtt_manager_publish(
                            MQTT_SENSOR_TOPIC,
                            json
                        );


                    if (ret != 0) {

                        printk(
                            "[APP] MQTT publish queue FAILED: %d\n",
                            ret
                        );
                    }
                }
            }
            else {

                printk(
                    "[APP] MQTT not ready - measurement not published\n"
                );
            }

             /* CLEAR SENSOR NEW-DATA FLAG */
            sensor_bme680_clear_new_data_flag();
        }
        k_sleep(
            K_MSEC(20)
        );
    }
    return 0;
}