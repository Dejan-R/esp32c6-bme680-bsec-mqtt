
#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <zephyr/types.h>
#include <stdbool.h>

typedef enum {
    WFM_STATE_DISCONNECTED,
    WFM_STATE_CONNECTING,
    WFM_STATE_CONNECTED,   
    WFM_STATE_READY       
} wifi_manager_state_t;

void wifi_manager_init(void);
wifi_manager_state_t wifi_manager_get_state(void);
bool wifi_manager_is_ready(void);

int wifi_manager_reconnect(void);

int wifi_manager_disconnect(void);

const char *wifi_manager_state_to_string(
    wifi_manager_state_t state
);

#endif 