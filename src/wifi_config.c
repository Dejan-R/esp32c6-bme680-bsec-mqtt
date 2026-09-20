#include "wifi_config.h"
#include "wifi_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include <string.h>
#include <errno.h>


#define WIFI_SETTINGS_ROOT "wifi"

static char wifi_ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
static char wifi_password[WIFI_CONFIG_PASS_MAX_LEN + 1];
K_MUTEX_DEFINE(wifi_config_mutex);


static int wifi_settings_set(const char *name,
                             size_t len,
                             settings_read_cb read_cb,
                             void *cb_arg)
{
    const char *next;
    int ret;

    if (settings_name_steq(name, "ssid", &next) && !next) {

        if (len > WIFI_CONFIG_SSID_MAX_LEN) {
            return -EINVAL;
        }

        memset(wifi_ssid, 0, sizeof(wifi_ssid));

        ret = read_cb(cb_arg, wifi_ssid, len);
        if (ret < 0) {
            return ret;
        }

        if ((size_t)ret != len) {
            return -EIO;
        }

        wifi_ssid[len] = '\0';

        return 0;
    }

    if (settings_name_steq(name, "password", &next) && !next) {

        if (len > WIFI_CONFIG_PASS_MAX_LEN) {
            return -EINVAL;
        }

        memset(wifi_password, 0, sizeof(wifi_password));

        ret = read_cb(cb_arg, wifi_password, len);
        if (ret < 0) {
            return ret;
        }

        if ((size_t)ret != len) {
            return -EIO;
        }

        wifi_password[len] = '\0';

        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(
    wifi,
    WIFI_SETTINGS_ROOT,
    NULL,
    wifi_settings_set,
    NULL,
    NULL
);


static bool wifi_config_is_valid_locked(void)
{
    size_t ssid_len = strnlen(wifi_ssid, sizeof(wifi_ssid));
    size_t password_len = strnlen(wifi_password, sizeof(wifi_password));

    return (ssid_len > 0U) &&
           (ssid_len <= WIFI_CONFIG_SSID_MAX_LEN) &&
           (password_len >= 8U) &&
           (password_len <= WIFI_CONFIG_PASS_MAX_LEN);
}


int wifi_config_init(void)
{
    int ret;

    k_mutex_lock(&wifi_config_mutex, K_FOREVER);

    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    memset(wifi_password, 0, sizeof(wifi_password));

    ret = settings_load_subtree(WIFI_SETTINGS_ROOT);

    if (ret != 0) {
        k_mutex_unlock(&wifi_config_mutex);
        printk("[WiFi Config] Failed to load settings: %d\n", ret);
        return ret;
    }

    bool valid = wifi_config_is_valid_locked();
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];

    if (valid) {
        memcpy(ssid, wifi_ssid, sizeof(ssid));
    }

    k_mutex_unlock(&wifi_config_mutex);

    if (valid) {
        printk("[WiFi Config] Configuration loaded\n");
        printk("[WiFi Config] SSID: %s\n", ssid);
    } else {
        printk("[WiFi Config] No WiFi configuration stored\n");
    }

    return 0;
}


bool wifi_config_is_valid(void)
{
    bool valid;

    k_mutex_lock(&wifi_config_mutex, K_FOREVER);
    valid = wifi_config_is_valid_locked();
    k_mutex_unlock(&wifi_config_mutex);

    return valid;
}


int wifi_config_get_credentials(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size)
{
    if ((ssid == NULL) || (password == NULL) ||
        (ssid_size == 0U) || (password_size == 0U)) {
        return -EINVAL;
    }

    k_mutex_lock(&wifi_config_mutex, K_FOREVER);

    if (!wifi_config_is_valid_locked()) {
        k_mutex_unlock(&wifi_config_mutex);
        return -ENODATA;
    }

    size_t ssid_len = strnlen(wifi_ssid, sizeof(wifi_ssid));
    size_t password_len = strnlen(wifi_password, sizeof(wifi_password));

    if ((ssid_len + 1U > ssid_size) ||
        (password_len + 1U > password_size)) {
        k_mutex_unlock(&wifi_config_mutex);
        return -ENOSPC;
    }

    memcpy(ssid, wifi_ssid, ssid_len + 1U);
    memcpy(password, wifi_password, password_len + 1U);

    k_mutex_unlock(&wifi_config_mutex);

    return 0;
}



int wifi_config_save(const char *ssid, const char *password)
{
    size_t ssid_len;
    size_t password_len;
    int ret;

    if ((ssid == NULL) || (password == NULL)) {
        return -EINVAL;
    }

    ssid_len = strlen(ssid);
    password_len = strlen(password);

    if ((ssid_len == 0U) ||
        (ssid_len > WIFI_CONFIG_SSID_MAX_LEN)) {
        return -EINVAL;
    }

    if ((password_len < 8U) ||
        (password_len > WIFI_CONFIG_PASS_MAX_LEN)) {
        return -EINVAL;
    }

    k_mutex_lock(&wifi_config_mutex, K_FOREVER);

    ret = settings_save_one(
        "wifi/ssid",
        ssid,
        ssid_len
    );

    if (ret != 0) {
        k_mutex_unlock(&wifi_config_mutex);
        return ret;
    }

    ret = settings_save_one(
        "wifi/password",
        password,
        password_len
    );

    if (ret != 0) {
        k_mutex_unlock(&wifi_config_mutex);
        return ret;
    }

    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    memset(wifi_password, 0, sizeof(wifi_password));

    memcpy(wifi_ssid, ssid, ssid_len);
    memcpy(wifi_password, password, password_len);

    k_mutex_unlock(&wifi_config_mutex);

    printk("[WiFi Config] Configuration saved\n");

    return 0;
}



int wifi_config_clear(void)
{
    int ret;

    k_mutex_lock(&wifi_config_mutex, K_FOREVER);

    ret = settings_delete("wifi/ssid");

    if ((ret != 0) && (ret != -ENOENT)) {
        k_mutex_unlock(&wifi_config_mutex);
        return ret;
    }

    ret = settings_delete("wifi/password");

    if ((ret != 0) && (ret != -ENOENT)) {
        k_mutex_unlock(&wifi_config_mutex);
        return ret;
    }

    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    memset(wifi_password, 0, sizeof(wifi_password));

    k_mutex_unlock(&wifi_config_mutex);

    printk("[WiFi Config] Configuration cleared\n");

    return 0;
}



static int cmd_wifi_status(const struct shell *sh,
                           size_t argc,
                           char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
    char password[WIFI_CONFIG_PASS_MAX_LEN + 1];

    int ret = wifi_config_get_credentials(
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    );

    shell_print(sh, "");
    shell_print(sh, "WiFi");
    shell_print(sh, "-----------");

    if (ret != 0) {
        shell_print(sh, "Configured : NO");
        shell_print(
            sh,
            "State      : %s",
            wifi_manager_state_to_string(
                wifi_manager_get_state()
            )
        );

        return 0;
    }

    shell_print(sh, "Configured : YES");
    shell_print(sh, "SSID       : %s", ssid);
    shell_print(sh, "Password   : ********");
    shell_print(
        sh,
        "State      : %s",
        wifi_manager_state_to_string(
            wifi_manager_get_state()
        )
    );

    return 0;
}



static int cmd_wifi_set(const struct shell *sh,
                        size_t argc,
                        char **argv)
{
    int ret;

    if (argc != 3) {

        shell_error(
            sh,
            "Usage: wifi set \"<ssid>\" \"<password>\""
        );

        return -EINVAL;
    }

    ret = wifi_config_save(argv[1], argv[2]);

    if (ret != 0) {

        shell_error(
            sh,
            "Failed to save WiFi configuration: %d",
            ret
        );

        return ret;
    }

    shell_print(sh, "WiFi configuration saved.");
    shell_print(sh, "SSID: %s", argv[1]);
    shell_print(sh, "Connecting...");

    ret = wifi_manager_reconnect();

    if (ret != 0) {

        shell_error(
            sh,
            "Failed to start WiFi reconnect: %d",
            ret
        );

        return ret;
    }

    return 0;
}


static int cmd_wifi_clear(const struct shell *sh,
                          size_t argc,
                          char **argv)
{
    int ret;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    ret = wifi_manager_disconnect();

    if ((ret != 0) && (ret != -ENOTCONN)) {

        shell_warn(
            sh,
            "WiFi disconnect returned: %d",
            ret
        );
    }

    ret = wifi_config_clear();

    if (ret != 0) {

        shell_error(
            sh,
            "Failed to clear WiFi configuration: %d",
            ret
        );

        return ret;
    }

    shell_print(sh, "WiFi configuration deleted.");
    shell_print(sh, "");
    shell_print(sh, "Configure a network with:");

    shell_print(
        sh,
        "wifi set \"<ssid>\" \"<password>\""
    );

    return 0;
}


static int cmd_wifi_reconnect(const struct shell *sh,
                              size_t argc,
                              char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
    char password[WIFI_CONFIG_PASS_MAX_LEN + 1];

    int ret = wifi_config_get_credentials(
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    );

    if (ret != 0) {
        shell_error(
            sh,
            "No WiFi configuration stored."
        );

        return -ENOENT;
    }

    shell_print(
        sh,
        "Reconnecting to: %s",
        ssid
    );

    ret = wifi_manager_reconnect();

    if (ret != 0) {
        shell_error(
            sh,
            "Reconnect failed: %d",
            ret
        );

        return ret;
    }

    return 0;
}



SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_wifi,

    SHELL_CMD(
        status,
        NULL,
        "Show WiFi status",
        cmd_wifi_status
    ),

    SHELL_CMD_ARG(
        set,
        NULL,
        "Set WiFi credentials: set \"<ssid>\" \"<password>\"",
        cmd_wifi_set,
        3,
        0
    ),

    SHELL_CMD(
        clear,
        NULL,
        "Delete stored WiFi credentials",
        cmd_wifi_clear
    ),

    SHELL_CMD(
        reconnect,
        NULL,
        "Reconnect WiFi",
        cmd_wifi_reconnect
    ),

    SHELL_SUBCMD_SET_END
);


SHELL_CMD_REGISTER(
    wifi,
    &sub_wifi,
    "WiFi configuration",
    NULL
);