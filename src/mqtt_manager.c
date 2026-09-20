#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>

#include <string.h>
#include <errno.h>

#include "mqtt_manager.h"
#include "wifi_manager.h"

#define MQTT_THREAD_STACK_SIZE      4096
#define MQTT_THREAD_PRIORITY        7
#define MQTT_RECONNECT_DELAY_SEC    5
#define MQTT_CONNECT_TIMEOUT_MS     15000
#define MQTT_SUBACK_TIMEOUT_MS      10000
#define MQTT_RX_PAYLOAD_MAX         127
#define MQTT_RX_TOPIC_MAX           127
#define MQTT_DISCARD_CHUNK_SIZE     64
#define MQTT_RX_TIMEOUT_MS          5000
#define MQTT_RX_RETRY_MS            10

typedef struct {
    char topic[64];
    char payload[512];
} mqtt_msg_t;

K_MSGQ_DEFINE(mqtt_msgq, sizeof(mqtt_msg_t), 8, 4);

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[1024];
static uint8_t tx_buffer[1024];
static struct mqtt_utf8 mqtt_user;
static struct mqtt_utf8 mqtt_pass;
static mqtt_manager_config_t mqtt_cfg;
static struct zsock_pollfd fds[1];
static atomic_t mqtt_state = ATOMIC_INIT(MQTT_STATE_DISCONNECTED);
static atomic_t reset_requested = ATOMIC_INIT(0);
static int64_t state_since_ms;
static uint16_t global_message_id;
static uint16_t subscription_message_id;
static bool client_active;
static mqtt_manager_message_cb_t message_callback;

#if defined(CONFIG_MQTT_LIB_TLS)
static sec_tag_t secure_tag_list[1];
#endif

static int connect_to_broker(void);
static int subscribe_topic(void);

static mqtt_manager_state_t get_state(void)
{
    return (mqtt_manager_state_t)atomic_get(&mqtt_state);
}

static void set_state(mqtt_manager_state_t state)
{
    atomic_set(&mqtt_state, (atomic_val_t)state);
    state_since_ms = k_uptime_get();
}

static uint16_t next_message_id(void)
{
    global_message_id++;

    if (global_message_id == 0U) {
        global_message_id = 1U;
    }

    return global_message_id;
}

static void request_reset(void)
{
    atomic_set(&reset_requested, 1);
}

static void mqtt_cleanup(void)
{
    if (client_active) {
        int ret = mqtt_abort(&client);

        if ((ret != 0) && (ret != -ENOTCONN)) {
            printk("[MQTT MGMT] mqtt_abort returned: %d\n", ret);
        }

        client_active = false;
    }

    fds[0].fd = -1;
    fds[0].events = 0;
    fds[0].revents = 0;

    k_msgq_purge(&mqtt_msgq);
}

static int read_publish_payload_until(
    struct mqtt_client *cl, uint8_t *buffer, size_t length,
    int64_t deadline_ms)
{
    size_t received = 0U;

    while (received < length) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();

        if (remaining_ms <= 0) {
            return -ETIMEDOUT;
        }

        int ret = mqtt_read_publish_payload(cl, buffer + received,
                                            length - received);

        if (ret > 0) {
            received += (size_t)ret;
            continue;
        }

        if (ret == 0) {
            return -ENOTCONN;
        }

        if ((ret != -EAGAIN) && (ret != -EWOULDBLOCK)) {
            return ret;
        }

        remaining_ms = deadline_ms - k_uptime_get();
        if (remaining_ms <= 0) {
            return -ETIMEDOUT;
        }

        k_sleep(K_MSEC(MIN(remaining_ms, (int64_t)MQTT_RX_RETRY_MS)));
    }

    return 0;
}

static int discard_publish_payload(
    struct mqtt_client *cl, size_t length, int64_t deadline_ms)
{
    uint8_t discard[MQTT_DISCARD_CHUNK_SIZE];
    size_t remaining = length;

    while (remaining > 0U) {
        size_t chunk = MIN(remaining, sizeof(discard));
        int ret = read_publish_payload_until(cl, discard, chunk, deadline_ms);

        if (ret != 0) {
            return ret;
        }

        remaining -= chunk;
    }

    return 0;
}

static int acknowledge_incoming_publish(
    struct mqtt_client *cl,
    const struct mqtt_publish_param *pub)
{
    if (pub->message.topic.qos != MQTT_QOS_1_AT_LEAST_ONCE) {
        return 0;
    }

    struct mqtt_puback_param ack = {
        .message_id = pub->message_id
    };

    int ret = mqtt_publish_qos1_ack(cl, &ack);

    if (ret != 0) {
        printk("[MQTT MGMT] Failed to send PUBACK: %d\n", ret);
    }

    return ret;
}

static int handle_incoming_publish(
    struct mqtt_client *cl,
    const struct mqtt_publish_param *pub)
{
    uint8_t payload[MQTT_RX_PAYLOAD_MAX + 1U];
    char topic[MQTT_RX_TOPIC_MAX + 1U];
    size_t payload_len = pub->message.payload.len;
    size_t topic_len = pub->message.topic.topic.size;
    int64_t deadline_ms = k_uptime_get() + MQTT_RX_TIMEOUT_MS;
    int ret;

    if (payload_len > MQTT_RX_PAYLOAD_MAX) {
        ret = discard_publish_payload(cl, payload_len, deadline_ms);

        if (ret != 0) {
            printk("[MQTT MGMT] Failed to discard oversized payload: %d\n", ret);
            return ret;
        }

        printk("[MQTT MGMT] RX payload too large (%u bytes), discarded\n",
               (unsigned int)payload_len);

        return acknowledge_incoming_publish(cl, pub);
    }

    if (payload_len > 0U) {
        ret = read_publish_payload_until(cl, payload, payload_len, deadline_ms);

        if (ret != 0) {
            printk("[MQTT MGMT] Failed to read publish payload: %d\n", ret);
            return ret;
        }
    }

    payload[payload_len] = '\0';

    if (topic_len > MQTT_RX_TOPIC_MAX) {
        printk("[MQTT MGMT] RX topic too long (%u bytes), message ignored\n",
               (unsigned int)topic_len);

        return acknowledge_incoming_publish(cl, pub);
    }

    memcpy(topic, pub->message.topic.topic.utf8, topic_len);
    topic[topic_len] = '\0';

    printk("[MQTT MGMT] RX Topic: %s, Payload: %s\n", topic, payload);

    if (message_callback != NULL) {
        message_callback(topic, (const char *)payload);
    }

    return acknowledge_incoming_publish(cl, pub);
}

static void mqtt_evt_handler(
    struct mqtt_client *const cl,
    const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            set_state(MQTT_STATE_CONNECTED);
            printk("[MQTT MGMT] CONNACK received. Subscribing...\n");

            int ret = subscribe_topic();

            if (ret != 0) {
                printk("[MQTT MGMT] Subscribe failed (%d)\n", ret);
                request_reset();
            }
        } else {
            printk("[MQTT MGMT] CONNACK error: %d\n", evt->result);
            request_reset();
        }
        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0) {
            printk("[MQTT MGMT] SUBACK error: %d\n", evt->result);
            request_reset();
            break;
        }

        if (evt->param.suback.message_id != subscription_message_id) {
            printk("[MQTT MGMT] Unexpected SUBACK message id: %u\n",
                   evt->param.suback.message_id);
            request_reset();
            break;
        }

        if ((evt->param.suback.return_codes.data == NULL) ||
            (evt->param.suback.return_codes.len == 0U) ||
            (evt->param.suback.return_codes.data[0] == MQTT_SUBACK_FAILURE)) {
            printk("[MQTT MGMT] Subscription rejected by broker\n");
            request_reset();
            break;
        }

        set_state(MQTT_STATE_READY);
        printk("[MQTT MGMT] SUBACK received. State -> READY\n");
        break;

    case MQTT_EVT_PUBLISH:
        if (handle_incoming_publish(cl, &evt->param.publish) != 0) {
            request_reset();
        }
        break;

    case MQTT_EVT_PUBACK:
        if (evt->result != 0) {
            printk("[MQTT MGMT] PUBACK error from broker: %d\n", evt->result);
        }
        break;

    case MQTT_EVT_PINGRESP:
        break;

    case MQTT_EVT_DISCONNECT:
        client_active = false;
        fds[0].fd = -1;
        fds[0].events = 0;
        fds[0].revents = 0;
        k_msgq_purge(&mqtt_msgq);
        set_state(MQTT_STATE_DISCONNECTED);
        printk("[MQTT MGMT] Disconnected (reason: %d)\n", evt->result);
        break;

    default:
        break;
    }
}

static int broker_init(void)
{
    struct zsock_addrinfo hints = {0};
    struct zsock_addrinfo *res = NULL;
    char port_str[6];

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintk(port_str, sizeof(port_str), "%u", mqtt_cfg.broker_port);

    printk("[MQTT MGMT] Resolving broker via DNS: %s:%s\n",
           mqtt_cfg.broker_address,
           port_str);

    int ret = zsock_getaddrinfo(
        mqtt_cfg.broker_address,
        port_str,
        &hints,
        &res
    );

    if (ret != 0) {
        printk("[MQTT MGMT] DNS resolution failed: %d\n", ret);
        return ret;
    }

    if ((res == NULL) || (res->ai_addrlen > sizeof(broker))) {
        if (res != NULL) {
            zsock_freeaddrinfo(res);
        }

        return -EOVERFLOW;
    }

    memset(&broker, 0, sizeof(broker));
    memcpy(&broker, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);

    printk("[MQTT MGMT] Broker resolved successfully.\n");
    return 0;
}

static int mqtt_setup(void)
{
    mqtt_client_init(&client);

    client.broker = &broker;
    client.evt_cb = mqtt_evt_handler;
    client.client_id.utf8 = (uint8_t *)mqtt_cfg.client_id;
    client.client_id.size = strlen(mqtt_cfg.client_id);

    if (mqtt_cfg.username != NULL) {
        mqtt_user.utf8 = (uint8_t *)mqtt_cfg.username;
        mqtt_user.size = strlen(mqtt_cfg.username);
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
    client.clean_session = 1U;
    client.keepalive = mqtt_cfg.keepalive;
    client.rx_buf = rx_buffer;
    client.rx_buf_size = sizeof(rx_buffer);
    client.tx_buf = tx_buffer;
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
        printk("[MQTT MGMT] ERROR: TLS requested but CONFIG_MQTT_LIB_TLS is disabled\n");
        return -ENOTSUP;
#endif
    } else {
        client.transport.type = MQTT_TRANSPORT_NON_SECURE;
    }

    return 0;
}

static int connect_to_broker(void)
{
    int ret = broker_init();

    if (ret != 0) {
        return ret;
    }

    ret = mqtt_setup();

    if (ret != 0) {
        return ret;
    }

    ret = mqtt_connect(&client);

    if (ret != 0) {
        printk("[MQTT MGMT] mqtt_connect failed: %d\n", ret);
        return ret;
    }

    client_active = true;

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
    return 0;
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

    subscription_message_id = next_message_id();

    struct mqtt_subscription_list list = {
        .list = &topic,
        .list_count = 1,
        .message_id = subscription_message_id,
    };

    return mqtt_subscribe(&client, &list);
}

static int execute_publish(const char *topic, const char *payload)
{
    struct mqtt_publish_param param = {0};

    param.message.topic.topic.utf8 = (uint8_t *)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.topic.qos = mqtt_cfg.qos;
    param.message.payload.data = (uint8_t *)payload;
    param.message.payload.len = strlen(payload);
    param.message_id =
        (mqtt_cfg.qos == MQTT_QOS_0_AT_MOST_ONCE) ? 0U : next_message_id();
    param.dup_flag = 0U;
    param.retain_flag = 0U;

    return mqtt_publish(&client, &param);
}

bool mqtt_manager_is_ready(void)
{
    return (get_state() == MQTT_STATE_READY);
}

void mqtt_manager_set_message_callback(mqtt_manager_message_cb_t callback)
{
    message_callback = callback;
}

mqtt_manager_state_t mqtt_manager_get_state(void)
{
    return get_state();
}

int mqtt_manager_publish(const char *topic, const char *payload)
{
    if ((topic == NULL) || (payload == NULL)) {
        return -EINVAL;
    }

    if (get_state() != MQTT_STATE_READY) {
        return -ENOTCONN;
    }

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);

    if ((topic_len == 0U) || (topic_len >= sizeof(((mqtt_msg_t *)0)->topic))) {
        return -EMSGSIZE;
    }

    if (payload_len >= sizeof(((mqtt_msg_t *)0)->payload)) {
        return -EMSGSIZE;
    }

    mqtt_msg_t msg;

    memcpy(msg.topic, topic, topic_len + 1U);
    memcpy(msg.payload, payload, payload_len + 1U);

    return k_msgq_put(&mqtt_msgq, &msg, K_NO_WAIT);
}

static bool state_timeout_expired(mqtt_manager_state_t state)
{
    int64_t elapsed = k_uptime_get() - state_since_ms;

    if (state == MQTT_STATE_CONNECTING) {
        return elapsed >= MQTT_CONNECT_TIMEOUT_MS;
    }

    if (state == MQTT_STATE_CONNECTED) {
        return elapsed >= MQTT_SUBACK_TIMEOUT_MS;
    }

    return false;
}

static void mqtt_manager_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    mqtt_msg_t outbound_msg;

    while (1) {
        if (!wifi_manager_is_ready()) {
            if ((get_state() != MQTT_STATE_DISCONNECTED) || client_active) {
                printk("[MQTT MGMT] WiFi lost. Resetting state...\n");
                mqtt_cleanup();
                set_state(MQTT_STATE_DISCONNECTED);
            }

            k_sleep(K_SECONDS(2));
            continue;
        }

        mqtt_manager_state_t state = get_state();

        if (state == MQTT_STATE_DISCONNECTED) {
            printk("[MQTT MGMT] Connecting to broker...\n");
            set_state(MQTT_STATE_CONNECTING);

            if (connect_to_broker() != 0) {
                mqtt_cleanup();
                set_state(MQTT_STATE_DISCONNECTED);
                k_sleep(K_SECONDS(MQTT_RECONNECT_DELAY_SEC));
                continue;
            }

            /*
             * DNS resolution and TCP/TLS setup can take a while.
             * Start the CONNACK timeout only after mqtt_connect()
             * has successfully initiated the MQTT connection.
             */
            set_state(MQTT_STATE_CONNECTING);
            continue;
        }

        if (state_timeout_expired(state)) {
            printk("[MQTT MGMT] State timeout while waiting for %s\n",
                   state == MQTT_STATE_CONNECTING ? "CONNACK" : "SUBACK");
            mqtt_cleanup();
            set_state(MQTT_STATE_DISCONNECTED);
            k_sleep(K_SECONDS(MQTT_RECONNECT_DELAY_SEC));
            continue;
        }

        int poll_ret = zsock_poll(fds, 1, 50);

        if ((poll_ret < 0) ||
            (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
            printk("[MQTT MGMT] Socket dead or disconnected. State -> DISCONNECTED\n");
            mqtt_cleanup();
            set_state(MQTT_STATE_DISCONNECTED);
            continue;
        }

        if ((poll_ret > 0) && (fds[0].revents & ZSOCK_POLLIN)) {
            int input_ret = mqtt_input(&client);

            if (input_ret != 0) {
                printk("[MQTT MGMT] mqtt_input failed (%d). State -> DISCONNECTED\n",
                       input_ret);
                mqtt_cleanup();
                set_state(MQTT_STATE_DISCONNECTED);
                continue;
            }
        }

        if (atomic_cas(&reset_requested, 1, 0)) {
            mqtt_cleanup();
            set_state(MQTT_STATE_DISCONNECTED);
            continue;
        }

        if (get_state() == MQTT_STATE_READY) {
            if (k_msgq_get(&mqtt_msgq, &outbound_msg, K_NO_WAIT) == 0) {
                int pub_ret = execute_publish(
                    outbound_msg.topic,
                    outbound_msg.payload
                );

                if (pub_ret != 0) {
                    printk("[MQTT MGMT] Publish failed: %d\n", pub_ret);
                }
            }

            int live_ret = mqtt_live(&client);

            if ((live_ret != 0) && (live_ret != -EAGAIN)) {
                printk("[MQTT MGMT] Keep-alive error (%d). State -> DISCONNECTED\n",
                       live_ret);
                mqtt_cleanup();
                set_state(MQTT_STATE_DISCONNECTED);
                continue;
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

    if ((cfg->broker_port == 0U) ||
        (cfg->keepalive == 0U) ||
        (cfg->broker_address == NULL) ||
        (cfg->client_id == NULL) ||
        (cfg->subscribe_topic == NULL)) {
        return -EINVAL;
    }

    if ((cfg->qos != MQTT_QOS_0_AT_MOST_ONCE) &&
        (cfg->qos != MQTT_QOS_1_AT_LEAST_ONCE)) {
        printk("[MQTT MGMT] Only QoS 0 and QoS 1 are supported by this manager.\n");
        return -ENOTSUP;
    }

    if (cfg->transport == MQTT_MANAGER_TRANSPORT_TLS) {
        if (cfg->tls_sec_tag < 0) {
            printk("[MQTT MGMT] TLS transport selected but tls_sec_tag is invalid.\n");
            return -EINVAL;
        }

#if !defined(CONFIG_MQTT_LIB_TLS)
        printk("[MQTT MGMT] TLS requested but CONFIG_MQTT_LIB_TLS is disabled.\n");
        return -ENOTSUP;
#endif
    }

    mqtt_cfg = *cfg;
    global_message_id = 0U;
    subscription_message_id = 0U;
    client_active = false;
    fds[0].fd = -1;
    fds[0].events = 0;
    fds[0].revents = 0;
    atomic_set(&reset_requested, 0);
    set_state(MQTT_STATE_DISCONNECTED);
    k_msgq_purge(&mqtt_msgq);

    k_thread_create(
        &mqtt_thread_data,
        mqtt_stack,
        K_THREAD_STACK_SIZEOF(mqtt_stack),
        mqtt_manager_thread,
        NULL,
        NULL,
        NULL,
        MQTT_THREAD_PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_name_set(&mqtt_thread_data, "mqtt_manager");

    printk("[MQTT MGMT] Initialized. Transport: %s, QoS: %d\n",
           cfg->transport == MQTT_MANAGER_TRANSPORT_TLS ? "TLS" : "TCP",
           cfg->qos);

    return 0;
}
