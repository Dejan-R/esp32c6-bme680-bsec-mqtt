#ifndef WIFI_CONFIG_H_
#define WIFI_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

#define WIFI_CONFIG_SSID_MAX_LEN 32
#define WIFI_CONFIG_PASS_MAX_LEN 63

int wifi_config_init(void);

bool wifi_config_is_valid(void);

int wifi_config_get_credentials(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size);

int wifi_config_save(const char *ssid, const char *password);
int wifi_config_clear(void);

#endif 