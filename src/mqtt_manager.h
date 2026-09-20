#ifndef MQTT_MANAGER_H_
#define MQTT_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_READY
} mqtt_manager_state_t;

typedef enum {
    MQTT_MANAGER_TRANSPORT_TCP,
    MQTT_MANAGER_TRANSPORT_TLS
} mqtt_manager_transport_t;

typedef struct {
    const char *broker_address;
    uint16_t broker_port;
    mqtt_manager_transport_t transport;

    const char *client_id;
    const char *username;
    const char *password;
    const char *subscribe_topic;

    uint16_t keepalive;
    enum mqtt_qos qos;  
    sec_tag_t tls_sec_tag;
} mqtt_manager_config_t;

typedef void (*mqtt_manager_message_cb_t)(const char *topic,
                                          const char *payload);

void mqtt_manager_set_message_callback(mqtt_manager_message_cb_t callback);

int mqtt_manager_init(const mqtt_manager_config_t *cfg);

bool mqtt_manager_is_ready(void);
mqtt_manager_state_t mqtt_manager_get_state(void);

int mqtt_manager_publish(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif

#endif
