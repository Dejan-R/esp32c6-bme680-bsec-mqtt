
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>
#include <string.h>
#include <errno.h>

#include "mqtt_manager.h"
#include "wifi_manager.h"

#define MQTT_THREAD_STACK_SIZE  4096
#define MQTT_THREAD_PRIORITY    7

typedef struct {
    char topic[64];
    char payload[512];
} mqtt_msg_t;
K_MSGQ_DEFINE(mqtt_msgq, sizeof(mqtt_msg_t), 8, 4);
static struct mqtt_client          client;
static struct sockaddr_storage     broker;
static uint8_t                     rx_buffer[1024];
static uint8_t                     tx_buffer[1024];
static struct mqtt_utf8            mqtt_user;
static struct mqtt_utf8            mqtt_pass;
static mqtt_manager_config_t       mqtt_cfg;
static struct zsock_pollfd         fds[1];
static volatile mqtt_manager_state_t mqtt_state = MQTT_STATE_DISCONNECTED;
static uint16_t                    global_message_id = 0;
static mqtt_manager_message_cb_t message_callback = NULL;
#if defined(CONFIG_MQTT_LIB_TLS)
static sec_tag_t secure_tag_list[1];
#endif
static int connect_to_broker(void);
static int subscribe_topic(void);
static void mqtt_cleanup(void)
{
#if defined(CONFIG_MQTT_LIB_TLS)
    if (client.transport.type == MQTT_TRANSPORT_SECURE) {
        if (client.transport.tls.sock >= 0) {
            zsock_close(client.transport.tls.sock);
            client.transport.tls.sock = -1;
        }
    } else
#endif
    {
        if (client.transport.tcp.sock >= 0) {
            zsock_close(client.transport.tcp.sock);
            client.transport.tcp.sock = -1;
        }
    }
}
static void mqtt_evt_handler(struct mqtt_client *const cl, const struct mqtt_evt *evt)
{
    switch (evt->type) {
case MQTT_EVT_CONNACK:
    if (evt->result == 0) {
        mqtt_state = MQTT_STATE_CONNECTED;
        printk("[MQTT MGMT] CONNACK received. Subscribing...\n");
        int ret = subscribe_topic();
        if (ret != 0) {
            printk("[MQTT MGMT] Subscribe failed (%d)\n", ret);
            mqtt_cleanup();
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
    } else {
        printk("[MQTT MGMT] CONNACK error: %d\n", evt->result);
        mqtt_cleanup();
        mqtt_state = MQTT_STATE_DISCONNECTED;
    }

    break;

    case MQTT_EVT_SUBACK:
        mqtt_state = MQTT_STATE_READY;
        printk("[MQTT MGMT] SUBACK received. State -> READY\n");
        break;

    case MQTT_EVT_PUBLISH:
        {
            const struct mqtt_publish_param *pub = &evt->param.publish;
            uint8_t payload[128];
            int len = MIN(pub->message.payload.len, sizeof(payload) - 1);
            int ret = mqtt_read_publish_payload(cl, payload, len);
            if (ret > 0) {
                payload[ret] = '\0';
                printk("[MQTT MGMT] RX Topic: %.*s, Payload: %s\n",
                       pub->message.topic.topic.size,
                       pub->message.topic.topic.utf8,
                       payload);

                       if (message_callback != NULL) {
    char topic[128];

    size_t topic_len = MIN(pub->message.topic.topic.size,
                           sizeof(topic) - 1);

    memcpy(topic,
           pub->message.topic.topic.utf8,
           topic_len);

    topic[topic_len] = '\0';

    message_callback(topic, (const char *)payload);
}

                       if (pub->message.topic.qos ==
    MQTT_QOS_1_AT_LEAST_ONCE) {
    struct mqtt_puback_param ack = {
        .message_id = pub->message_id
    };
    mqtt_publish_qos1_ack(cl, &ack);
}
            }
        }
        break;
    case MQTT_EVT_PUBACK:
        if (evt->result == 0) {
            printk("[MQTT MGMT] PUBACK received from broker (msg_id: %d)\n",
                   evt->param.puback.message_id);
        } else {
            printk("[MQTT MGMT] PUBACK error from broker: %d\n", evt->result);
        }
        break;
    case MQTT_EVT_PINGRESP:
        break;
    case MQTT_EVT_DISCONNECT:
     mqtt_cleanup();
mqtt_state = MQTT_STATE_DISCONNECTED;
        printk("[MQTT MGMT] Disconnected (reason: %d)\n", evt->result);
        break;
    default:
        break;
    }
}

static int broker_init(void)
{
    struct zsock_addrinfo hints = {0};
    struct zsock_addrinfo *res;
    char port_str[6];
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintk(port_str, sizeof(port_str), "%d", mqtt_cfg.broker_port);
    printk("[MQTT MGMT] Resolving broker via DNS: %s:%s\n", mqtt_cfg.broker_address, port_str);
    int ret = zsock_getaddrinfo(mqtt_cfg.broker_address, port_str, &hints, &res);
    if (ret != 0) {
        printk("[MQTT MGMT] DNS resolution failed: %d\n", ret);
        return ret;
    }
    memcpy(&broker, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    printk("[MQTT MGMT] Broker resolved successfully.\n");
    return 0;
}

static void mqtt_setup(void)
{
    mqtt_client_init(&client);
    client.broker   = &broker;
    client.evt_cb   = mqtt_evt_handler;
    client.client_id.utf8 = (uint8_t *)mqtt_cfg.client_id;
    client.client_id.size = strlen(mqtt_cfg.client_id);

    if (mqtt_cfg.username != NULL) {
        mqtt_user.utf8   = (uint8_t *)mqtt_cfg.username;
        mqtt_user.size   = strlen(mqtt_cfg.username);
        client.user_name = &mqtt_user;
    } else {
        client.user_name = NULL;
    }
    if (mqtt_cfg.password != NULL) {
        mqtt_pass.utf8 = (uint8_t *)mqtt_cfg.password;
        mqtt_pass.size = strlen(mqtt_cfg.password);
        client.password = &mqtt_pass;
    } else {
        client.password = NULL;
    }

    client.protocol_version = MQTT_VERSION_3_1_1;
    client.clean_session    = 1U;
    client.keepalive        = mqtt_cfg.keepalive;
    client.rx_buf      = rx_buffer;
    client.rx_buf_size = sizeof(rx_buffer);
    client.tx_buf      = tx_buffer;
    client.tx_buf_size = sizeof(tx_buffer);
    if (mqtt_cfg.transport == MQTT_MANAGER_TRANSPORT_TLS) {
#if defined(CONFIG_MQTT_LIB_TLS)
        secure_tag_list[0] = mqtt_cfg.tls_sec_tag;

        client.transport.type = MQTT_TRANSPORT_SECURE;
        client.transport.tls.config.peer_verify = TLS_PEER_VERIFY_REQUIRED;
        client.transport.tls.config.sec_tag_list = secure_tag_list;
        client.transport.tls.config.sec_tag_count = 1;
        client.transport.tls.config.hostname = mqtt_cfg.broker_address;
#else
        printk("[MQTT MGMT] ERROR: TLS requested but CONFIG_MQTT_LIB_TLS is missing or deactivated in build!\n");
        client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
    } else {
        client.transport.type = MQTT_TRANSPORT_NON_SECURE;
    }
}

static int connect_to_broker(void)
{
    int ret = broker_init();
    if (ret != 0) {
        return ret;
    }
    mqtt_setup();
    ret = mqtt_connect(&client);
    if (ret == 0) {
#if defined(CONFIG_MQTT_LIB_TLS)
        if (client.transport.type == MQTT_TRANSPORT_SECURE) {
            fds[0].fd = client.transport.tls.sock;
        } else {
            fds[0].fd = client.transport.tcp.sock;
        }
#else
        fds[0].fd = client.transport.tcp.sock;
#endif
        fds[0].events = ZSOCK_POLLIN;
        fds[0].revents = 0;
        printk("[MQTT MGMT] Connection initiated. Waiting for CONNACK...\n");
    } else {
        printk("[MQTT MGMT] mqtt_connect failed: %d\n", ret);
    }
    return ret;
}

static int subscribe_topic(void)
{
    struct mqtt_topic topic = {
        .topic = {
            .utf8 = (uint8_t *)mqtt_cfg.subscribe_topic,
            .size = strlen(mqtt_cfg.subscribe_topic),
        },
        .qos = mqtt_cfg.qos,
    };
    struct mqtt_subscription_list list = {
        .list       = &topic,
        .list_count = 1,
        .message_id = ++global_message_id,
    };
    return mqtt_subscribe(&client, &list);
}

static int execute_publish(const char *topic, const char *payload)
{
    struct mqtt_publish_param param = {0};
    param.message.topic.topic.utf8 = (uint8_t *)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.topic.qos        = mqtt_cfg.qos;
    param.message.payload.data     = (uint8_t *)payload;
    param.message.payload.len      = strlen(payload);
    param.message_id               = ++global_message_id;
    param.dup_flag                 = 0U;
    param.retain_flag              = 0U;
    return mqtt_publish(&client, &param);
}

bool mqtt_manager_is_ready(void)
{
    return (mqtt_state == MQTT_STATE_READY);
}

void mqtt_manager_set_message_callback(mqtt_manager_message_cb_t callback)
{
    message_callback = callback;
}

mqtt_manager_state_t mqtt_manager_get_state(void)
{
    return mqtt_state;
}
int mqtt_manager_publish(const char *topic, const char *payload)
{
    if (mqtt_state != MQTT_STATE_READY) {
        return -ENOTCONN;
    }
    mqtt_msg_t msg;
    snprintk(msg.topic,   sizeof(msg.topic),   "%s", topic);
    snprintk(msg.payload, sizeof(msg.payload),  "%s", payload);
    return k_msgq_put(&mqtt_msgq, &msg, K_NO_WAIT);
}

static void mqtt_manager_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    mqtt_msg_t outbound_msg;
    int input_ret;
    while (1) {
        if (!wifi_manager_is_ready()) {
            if (mqtt_state != MQTT_STATE_DISCONNECTED) {
                printk("[MQTT MGMT] WiFi lost. Resetting state...\n");
                mqtt_cleanup();
                mqtt_state = MQTT_STATE_DISCONNECTED;
            }
            k_sleep(K_SECONDS(2));
            continue;
        }
        switch (mqtt_state) {
        case MQTT_STATE_DISCONNECTED:
            printk("[MQTT MGMT] Connecting to broker...\n");
            mqtt_state = MQTT_STATE_CONNECTING;
            if (connect_to_broker() != 0) {
                      mqtt_cleanup();
                mqtt_state = MQTT_STATE_DISCONNECTED;
                k_sleep(K_SECONDS(5));
            }
            break;
        case MQTT_STATE_CONNECTING:
        case MQTT_STATE_CONNECTED:
        case MQTT_STATE_READY:
        {
            int poll_ret = zsock_poll(fds, 1, 50);
            if (poll_ret < 0 || (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
                printk("[MQTT MGMT] Socket dead or disconnected. State -> DISCONNECTED\n");
                    mqtt_cleanup();
                mqtt_state = MQTT_STATE_DISCONNECTED;
                break;
            }
            if (poll_ret > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
                input_ret = mqtt_input(&client);
                if (input_ret != 0) {
                    printk("[MQTT MGMT] mqtt_input failed (%d). State -> DISCONNECTED\n", input_ret);
                    mqtt_cleanup();
                    mqtt_state = MQTT_STATE_DISCONNECTED;
                    break;
                }
            }
            if (mqtt_state == MQTT_STATE_READY) {
                if (k_msgq_get(&mqtt_msgq, &outbound_msg, K_NO_WAIT) == 0) {
                    int pub_ret = execute_publish(outbound_msg.topic, outbound_msg.payload);
                    if (pub_ret != 0) {
                        printk("[MQTT MGMT] Publish failed: %d\n", pub_ret);
                    }
                }
                int live_ret = mqtt_live(&client);
                if (live_ret != 0 && live_ret != -EAGAIN) {
                    printk("[MQTT MGMT] Keep-alive error (%d). State -> DISCONNECTED\n", live_ret);
                     mqtt_cleanup();
                    mqtt_state = MQTT_STATE_DISCONNECTED;
                }
            }
            break;
        }
        }
        k_sleep(K_MSEC(50));
    }
}

K_THREAD_STACK_DEFINE(mqtt_stack, MQTT_THREAD_STACK_SIZE);
static struct k_thread mqtt_thread_data;
int mqtt_manager_init(const mqtt_manager_config_t *cfg)
{
    if (cfg == NULL) {
        return -EINVAL;
    }
    if (cfg->broker_port == 0      ||
        cfg->keepalive   == 0      ||
        cfg->broker_address  == NULL ||
        cfg->client_id       == NULL ||
        cfg->subscribe_topic == NULL) {
        return -EINVAL;
    }
    if (cfg->transport == MQTT_MANAGER_TRANSPORT_TLS && cfg->tls_sec_tag < 0) {
        printk("[MQTT MGMT] TLS transport selected but tls_sec_tag is invalid.\n");
        return -EINVAL;
    }
    mqtt_cfg = *cfg;
    k_thread_create(&mqtt_thread_data, mqtt_stack,
                    K_THREAD_STACK_SIZEOF(mqtt_stack),
                    mqtt_manager_thread, NULL, NULL, NULL,
                    MQTT_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&mqtt_thread_data, "mqtt_manager");
    printk("[MQTT MGMT] Initialized. Transport: %s, QoS: %d\n",
           cfg->transport == MQTT_MANAGER_TRANSPORT_TLS ? "TLS" : "TCP", cfg->qos);
    return 0;
}