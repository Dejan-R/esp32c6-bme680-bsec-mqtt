
#include "wifi_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/sys/printk.h>
#include <string.h>


#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define RECONNECT_DELAY_SEC 5

static wifi_manager_state_t current_state = WFM_STATE_DISCONNECTED;

static struct net_if *wifi_iface;

static struct net_mgmt_event_callback l2_cb;
static struct net_mgmt_event_callback l4_cb;

static struct k_work_delayable connect_work;

static void set_state(wifi_manager_state_t next_state)
{
    const char *state_names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "READY"};
    printk("[WiFi Mgr] State transition: %s -> %s\n", state_names[current_state], state_names[next_state]);
    current_state = next_state;
}

static void connect_work_handler(struct k_work *work)
{
    if (current_state == WFM_STATE_READY || current_state == WFM_STATE_CONNECTED) {
        return;
    }

    set_state(WFM_STATE_CONNECTING);
    printk("[WiFi Mgr] Submitting async connect request to SSID: %s...\n", WIFI_SSID);

    struct wifi_connect_req_params wifi_params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PASS,
        .psk_length = strlen(WIFI_PASS),
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .mfp = WIFI_MFP_OPTIONAL,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &wifi_params, sizeof(wifi_params));
    if (ret) {
        printk("[WiFi Mgr] Connect request failed to submit (%d). Retrying...\n", ret);
        set_state(WFM_STATE_DISCONNECTED);
        k_work_reschedule(&connect_work, K_SECONDS(RECONNECT_DELAY_SEC));
    }
}

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{  
    if (iface != wifi_iface) {
        return;
    }

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        if (status->status == 0) {
            printk("[WiFi Mgr] L2 Connected (Link Up).\n");
            
            /*
             * L4_CONNECTED may arrive before WIFI_CONNECT_RESULT.
            * Do not move the state back from READY to CONNECTED.
            */
            if (current_state != WFM_STATE_READY) {
                set_state(WFM_STATE_CONNECTED);
            }
        } else {
            printk("[WiFi Mgr] L2 Connection failed (%d). Scheduling retry...\n", status->status);
            set_state(WFM_STATE_DISCONNECTED);
            k_work_reschedule(&connect_work, K_SECONDS(RECONNECT_DELAY_SEC));
        }
    }
    else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        printk("[WiFi Mgr] L2 Disconnected (Link Down)! Triggering reconnect...\n");
        set_state(WFM_STATE_DISCONNECTED);
        k_work_reschedule(&connect_work, K_SECONDS(RECONNECT_DELAY_SEC));
    }
    
    else if (event == NET_EVENT_L4_CONNECTED) {
        printk("[WiFi Mgr] L4 Connected! IP Address is active and valid.\n");
       
        set_state(WFM_STATE_READY);
    }
    else if (event == NET_EVENT_L4_DISCONNECTED) {
        printk("[WiFi Mgr] L4 Lost (IP lost)! Moving back to disconnected...\n");
        set_state(WFM_STATE_DISCONNECTED);
        k_work_reschedule(&connect_work, K_SECONDS(RECONNECT_DELAY_SEC));
    }
}

void wifi_manager_init(void)
{
    wifi_iface = net_if_get_default();
    if (!wifi_iface) {
        printk("[WiFi Mgr] ERROR: No default interface found!\n");
        return;
    }

    k_work_init_delayable(&connect_work, connect_work_handler);

    net_mgmt_init_event_callback(&l2_cb, net_event_handler, 
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&l2_cb);

    net_mgmt_init_event_callback(&l4_cb, net_event_handler, 
                                 NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
    net_mgmt_add_event_callback(&l4_cb);
    net_if_up(wifi_iface);
    k_work_reschedule(&connect_work, K_SECONDS(2));
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return current_state;
}

bool wifi_manager_is_ready(void)
{
    return (current_state == WFM_STATE_READY);
}