#include "wifi_manager.h"
#include "wifi_config.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <string.h>
#include <errno.h>


#define RECONNECT_DELAY_SEC 5
#define CONNECT_TIMEOUT_SEC 20


static atomic_t current_state = ATOMIC_INIT(WFM_STATE_DISCONNECTED);
static atomic_t auto_reconnect = ATOMIC_INIT(1);

static struct net_if *wifi_iface;

static struct net_mgmt_event_callback l2_cb;
static struct net_mgmt_event_callback l4_cb;

static struct k_work_delayable connect_work;
static struct k_work_delayable connect_timeout_work;

static char connect_ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
static char connect_password[WIFI_CONFIG_PASS_MAX_LEN + 1];


static wifi_manager_state_t get_state(void)
{
    return (wifi_manager_state_t)atomic_get(&current_state);
}


static bool auto_reconnect_enabled(void)
{
    return atomic_get(&auto_reconnect) != 0;
}


static void set_auto_reconnect(bool enabled)
{
    atomic_set(&auto_reconnect, enabled ? 1 : 0);
}


static const char *state_to_string(wifi_manager_state_t state)
{
    switch (state) {
    case WFM_STATE_DISCONNECTED:
        return "DISCONNECTED";

    case WFM_STATE_CONNECTING:
        return "CONNECTING";

    case WFM_STATE_CONNECTED:
        return "CONNECTED";

    case WFM_STATE_READY:
        return "READY";

    default:
        return "UNKNOWN";
    }
}


static void set_state(wifi_manager_state_t next_state)
{
    wifi_manager_state_t previous_state = get_state();

    atomic_set(&current_state, (atomic_val_t)next_state);

    if (previous_state != next_state) {
        printk(
            "[WiFi Mgr] State transition: %s -> %s\n",
            state_to_string(previous_state),
            state_to_string(next_state)
        );
    }
}


static void schedule_reconnect(k_timeout_t delay)
{
    if (!auto_reconnect_enabled()) {
        return;
    }

    k_work_reschedule(&connect_work, delay);
}


static void connect_timeout_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (get_state() != WFM_STATE_CONNECTING) {
        return;
    }

    printk(
        "[WiFi Mgr] Connect timeout after %d seconds.\n",
        CONNECT_TIMEOUT_SEC
    );

    if (wifi_iface != NULL) {
        int ret = net_mgmt(
            NET_REQUEST_WIFI_DISCONNECT,
            wifi_iface,
            NULL,
            0
        );

        if ((ret != 0) && (ret != -ENOTCONN)) {
            printk(
                "[WiFi Mgr] Disconnect after timeout returned: %d\n",
                ret
            );
        }
    }

    set_state(WFM_STATE_DISCONNECTED);
    schedule_reconnect(K_SECONDS(RECONNECT_DELAY_SEC));
}


static void connect_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!auto_reconnect_enabled()) {
        return;
    }

    wifi_manager_state_t state = get_state();

    if ((state == WFM_STATE_READY) ||
        (state == WFM_STATE_CONNECTED) ||
        (state == WFM_STATE_CONNECTING)) {
        return;
    }

    int ret = wifi_config_get_credentials(
        connect_ssid,
        sizeof(connect_ssid),
        connect_password,
        sizeof(connect_password)
    );

    if (ret != 0) {
        printk("[WiFi Mgr] No valid WiFi configuration stored.\n");
        set_state(WFM_STATE_DISCONNECTED);
        return;
    }

    set_state(WFM_STATE_CONNECTING);

    printk(
        "[WiFi Mgr] Submitting async connect request to SSID: %s...\n",
        connect_ssid
    );

    struct wifi_connect_req_params wifi_params = {
        .ssid = (const uint8_t *)connect_ssid,
        .ssid_length = strlen(connect_ssid),
        .psk = (const uint8_t *)connect_password,
        .psk_length = strlen(connect_password),
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .mfp = WIFI_MFP_OPTIONAL,
    };

    ret = net_mgmt(
        NET_REQUEST_WIFI_CONNECT,
        wifi_iface,
        &wifi_params,
        sizeof(wifi_params)
    );

    if (ret != 0) {
        printk(
            "[WiFi Mgr] Connect request failed to submit (%d).\n",
            ret
        );

        set_state(WFM_STATE_DISCONNECTED);
        schedule_reconnect(K_SECONDS(RECONNECT_DELAY_SEC));
        return;
    }

    k_work_reschedule(
        &connect_timeout_work,
        K_SECONDS(CONNECT_TIMEOUT_SEC)
    );
}


static void net_event_handler(
    struct net_mgmt_event_callback *cb,
    uint64_t event,
    struct net_if *iface)
{
    if (iface != wifi_iface) {
        return;
    }

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;

        k_work_cancel_delayable(&connect_timeout_work);

        if (status->status == 0) {
            printk("[WiFi Mgr] L2 Connected (Link Up).\n");

            /*
             * L4_CONNECTED may arrive before WIFI_CONNECT_RESULT.
             * Do not move the state back from READY to CONNECTED.
             */
            if (get_state() != WFM_STATE_READY) {
                set_state(WFM_STATE_CONNECTED);
            }

        } else {
            printk(
                "[WiFi Mgr] L2 Connection failed (%d).\n",
                status->status
            );

            set_state(WFM_STATE_DISCONNECTED);
            schedule_reconnect(K_SECONDS(RECONNECT_DELAY_SEC));
        }
    }

    else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        k_work_cancel_delayable(&connect_timeout_work);

        printk("[WiFi Mgr] L2 Disconnected (Link Down).\n");

        set_state(WFM_STATE_DISCONNECTED);
        schedule_reconnect(K_SECONDS(RECONNECT_DELAY_SEC));
    }

    else if (event == NET_EVENT_L4_CONNECTED) {
        k_work_cancel_delayable(&connect_timeout_work);

        printk(
            "[WiFi Mgr] L4 Connected! IP Address is active and valid.\n"
        );

        set_state(WFM_STATE_READY);
    }

    else if (event == NET_EVENT_L4_DISCONNECTED) {
        k_work_cancel_delayable(&connect_timeout_work);

        printk(
            "[WiFi Mgr] L4 Lost (IP lost)! Moving back to disconnected...\n"
        );

        set_state(WFM_STATE_DISCONNECTED);
        schedule_reconnect(K_SECONDS(RECONNECT_DELAY_SEC));
    }
}


void wifi_manager_init(void)
{
    wifi_iface = net_if_get_default();

    if (wifi_iface == NULL) {
        printk("[WiFi Mgr] ERROR: No default interface found!\n");
        return;
    }

    k_work_init_delayable(
        &connect_work,
        connect_work_handler
    );

    k_work_init_delayable(
        &connect_timeout_work,
        connect_timeout_handler
    );

    net_mgmt_init_event_callback(
        &l2_cb,
        net_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT |
        NET_EVENT_WIFI_DISCONNECT_RESULT
    );

    net_mgmt_add_event_callback(&l2_cb);

    net_mgmt_init_event_callback(
        &l4_cb,
        net_event_handler,
        NET_EVENT_L4_CONNECTED |
        NET_EVENT_L4_DISCONNECTED
    );

    net_mgmt_add_event_callback(&l4_cb);

    set_state(WFM_STATE_DISCONNECTED);

    net_if_up(wifi_iface);

    if (wifi_config_is_valid()) {
        set_auto_reconnect(true);

        printk("[WiFi Mgr] Stored WiFi configuration found.\n");

        k_work_reschedule(
            &connect_work,
            K_SECONDS(2)
        );

    } else {
        set_auto_reconnect(false);

        printk("[WiFi Mgr] No WiFi configuration stored.\n");
        printk("[WiFi Mgr] Use shell command:\n");
        printk("[WiFi Mgr] wifi set \"SSID\" \"PASSWORD\"\n");
    }
}


int wifi_manager_disconnect(void)
{
    set_auto_reconnect(false);

    k_work_cancel_delayable(&connect_work);
    k_work_cancel_delayable(&connect_timeout_work);

    if (wifi_iface == NULL) {
        return -ENODEV;
    }

    if (get_state() == WFM_STATE_DISCONNECTED) {
        return 0;
    }

    printk("[WiFi Mgr] Disconnecting...\n");

    int ret = net_mgmt(
        NET_REQUEST_WIFI_DISCONNECT,
        wifi_iface,
        NULL,
        0
    );

    if (ret == -ENOTCONN) {
        set_state(WFM_STATE_DISCONNECTED);
        return 0;
    }

    return ret;
}


int wifi_manager_reconnect(void)
{
    if (!wifi_config_is_valid()) {
        printk(
            "[WiFi Mgr] Cannot reconnect: WiFi is not configured.\n"
        );
        return -ENOENT;
    }

    if (wifi_iface == NULL) {
        return -ENODEV;
    }

    set_auto_reconnect(false);

    k_work_cancel_delayable(&connect_work);
    k_work_cancel_delayable(&connect_timeout_work);

    if (get_state() != WFM_STATE_DISCONNECTED) {
        printk("[WiFi Mgr] Disconnecting current network...\n");

        int ret = net_mgmt(
            NET_REQUEST_WIFI_DISCONNECT,
            wifi_iface,
            NULL,
            0
        );

        if ((ret != 0) && (ret != -ENOTCONN)) {
            printk(
                "[WiFi Mgr] Disconnect request returned: %d\n",
                ret
            );
        }

        k_sleep(K_MSEC(500));
    }

    set_state(WFM_STATE_DISCONNECTED);
    set_auto_reconnect(true);

    printk(
        "[WiFi Mgr] Connecting using current configuration...\n"
    );

    k_work_reschedule(
        &connect_work,
        K_MSEC(100)
    );

    return 0;
}


const char *wifi_manager_state_to_string(
    wifi_manager_state_t state)
{
    return state_to_string(state);
}


wifi_manager_state_t wifi_manager_get_state(void)
{
    return get_state();
}


bool wifi_manager_is_ready(void)
{
    return get_state() == WFM_STATE_READY;
}
