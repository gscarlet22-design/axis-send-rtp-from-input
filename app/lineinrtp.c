/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * Captures the C8110 line input and sends unicast G.711 A-law RTP while the
 * level stays above a threshold. Audio Manager Pro RTP sources listen; this
 * app is the sender. Packets stop when the input goes quiet so the server's
 * activity detector sees the source go idle.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include <axsdk/axevent.h>
#include <axsdk/axparameter.h>
#include <glib.h>
#include <jansson.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#pragma GCC diagnostic pop

#define APP_NAME "lineinrtp"
#define HTTP_PORT 2011
#define NODE_MAX 32
#define NAME_MAX_LEN 63
#define RING_CAP 1600U
#define RTP_SAMPLES 160U
#define DEFAULT_NODE "AudioDevice0Input0.Unprocessed"

struct cfg {
    bool enabled;
    bool force_on;
    bool test_mode;
    int trigger_port;
    char destination_ip[64];
    int destination_port;
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
    char input_node[NAME_MAX_LEN + 1];
};

struct pending_cfg {
    bool have;
    bool have_alarm;
    int alarm_level;
    struct cfg cfg;
};

struct status_snap {
    double level_db;
    char gate[16];
    bool streaming;
    bool enabled;
    bool force_on;
    bool test_mode;
    bool node_connected;
    int trigger_port;
    int port1_active;
    int port2_active;
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
    int destination_port;
    int device_alarm_level;
    double level_percent;
    int capture_rate;
    int capture_channels;
    char destination_ip[64];
    char stream_url[96];
    char input_node[NAME_MAX_LEN + 1];
    char connected_node[NAME_MAX_LEN + 1];
    char last_error[160];
    char last_rtp_header[25];
    uint64_t packets_sent;
    uint64_t send_errors;
    uint64_t samples_dropped;
    unsigned buffered_samples;
};

struct node_ref {
    bool used;
    uint32_t id;
    char name[NAME_MAX_LEN + 1];
};

struct impl {
    struct pw_main_loop* loop;
    struct pw_context* context;
    struct pw_core* core;
    struct pw_registry* registry;
    struct spa_hook registry_listener;
    struct spa_source* timer_source;
    struct pw_stream* stream;
    struct spa_hook stream_listener;
    struct spa_audio_info info;
    bool format_ready;
    struct node_ref nodes[NODE_MAX];
    char connected_name[NAME_MAX_LEN + 1];
    uint32_t connected_id;
    AXParameter* params;
    int http_listen_fd;
    pthread_t http_thread;
};

static struct impl g_impl;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct pending_cfg g_pending;
static struct status_snap g_status;
static struct cfg g_cfg;
static int64_t g_hot_since;
static int64_t g_quiet_since;
static int64_t g_active_since;
static bool g_streaming;
static bool g_waiting_for_clear;
static bool g_marker = true;
static double g_level_db = -120.0;
static double g_level_percent;
static int g_alarm_level = -1;
static int g_decim;
static float g_acc;
static int g_acc_n;
static int16_t g_ring[RING_CAP];
static unsigned g_ring_r;
static unsigned g_ring_w;
static unsigned g_ring_count;
static int g_rtp_fd = -1;
static char g_rtp_ip[64];
static int g_rtp_port;
static uint16_t g_seq;
static uint32_t g_timestamp;
static uint32_t g_ssrc;
static uint64_t g_packets_sent;
static uint64_t g_send_errors;
static uint64_t g_samples_dropped;
static char g_last_error[160];
static char g_last_rtp_header[25];
static bool g_logged_bad_format;
static double g_tone_phase;
static volatile sig_atomic_t g_port_active[3] = {-1, -1, -1};
static volatile sig_atomic_t g_port_numbering;
static GMainLoop* g_event_loop;
static pthread_t g_event_thread;
static bool g_event_started;
static gint g_event_ready;
static gint g_event_stop;
static int64_t g_last_cfg_ms;
static bool g_pw_inited;
static bool g_registry_hooked;
static int port_is_active(int which);
static int64_t g_last_report_ms;

static void publish_status(const char* gate);
static void ensure_stream(struct impl* impl);
static int64_t now_ms(void);

static int64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_error(const char* text) {
    snprintf(g_last_error, sizeof g_last_error, "%s", text != NULL ? text : "");
}

/* ITU-T G.711 A-law, 13-bit linear magnitude to 8-bit. */
static uint8_t linear_to_alaw(int16_t pcm) {
    static const int16_t seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int mask;
    int seg;
    int aval;
    int value;
    int i;

    value = pcm >> 3;
    if (value >= 0) {
        mask = 0xD5;
    } else {
        mask  = 0x55;
        value = -value - 1;
        if (value < 0) {
            value = 0;
        }
    }
    seg = 8;
    for (i = 0; i < 8; i++) {
        if (value <= seg_end[i]) {
            seg = i;
            break;
        }
    }
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    aval = seg << 4;
    if (seg < 2) {
        aval |= (value >> 1) & 0x0F;
    } else {
        aval |= (value >> seg) & 0x0F;
    }
    return (uint8_t)(aval ^ mask);
}

/* Canonical dotted IPv4, and a unicast target. Rejects leading zeros, 0.0.0.0/8,
 * multicast 224.0.0.0/4, and the reserved 240.0.0.0/4 block. */
static bool valid_ipv4(const char* text) {
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    char tail;
    char canonical[16];

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255 || a == 0 || a >= 224) {
        return false;
    }
    snprintf(canonical, sizeof canonical, "%u.%u.%u.%u", a, b, c, d);
    return strcmp(text, canonical) == 0;
}

static bool valid_node_name(const char* text) {
    size_t i;
    size_t n;

    if (text == NULL) {
        return false;
    }
    n = strlen(text);
    if (n == 0 || n > NAME_MAX_LEN) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char ch = text[i];
        bool ok  = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static int param_int(AXParameter* handle, const char* name, int fallback, int min_v, int max_v) {
    GError* error = NULL;
    gchar* value  = NULL;
    int out       = fallback;

    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        char* end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end != value && end != NULL && *end == '\0' && parsed >= min_v && parsed <= max_v) {
            out = (int)parsed;
        }
    }
    g_free(value);
    g_clear_error(&error);
    return out;
}

static bool param_yes(AXParameter* handle, const char* name, bool fallback) {
    GError* error = NULL;
    gchar* value  = NULL;
    bool out      = fallback;

    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        out = strcmp(value, "yes") == 0;
    }
    g_free(value);
    g_clear_error(&error);
    return out;
}

static void param_str(AXParameter* handle, const char* name, char* dest, size_t dest_len, const char* fallback) {
    GError* error = NULL;
    gchar* value  = NULL;

    snprintf(dest, dest_len, "%s", fallback != NULL ? fallback : "");
    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        snprintf(dest, dest_len, "%s", value);
    }
    g_free(value);
    g_clear_error(&error);
}

static void load_cfg(struct impl* impl) {
    struct cfg next;

    memset(&next, 0, sizeof next);
    if (impl->params == NULL) {
        next.destination_port    = 49000;
        next.threshold_db        = -45;
        next.margin_db           = 6;
        next.activation_delay_ms = 200;
        next.deactivation_delay_s = 2;
        snprintf(next.input_node, sizeof next.input_node, "%s", DEFAULT_NODE);
        g_cfg = next;
        return;
    }
    next.enabled              = param_yes(impl->params, "Enabled", false);
    next.force_on             = param_yes(impl->params, "ForceOn", false);
    next.test_mode            = param_yes(impl->params, "TestMode", false);
    {
        char trigger[16];
        param_str(impl->params, "Trigger", trigger, sizeof trigger, "level");
        if (strcmp(trigger, "port1") == 0) {
            next.trigger_port = 1;
        } else if (strcmp(trigger, "port2") == 0) {
            next.trigger_port = 2;
        } else {
            next.trigger_port = 0;
        }
    }
    next.destination_port     = param_int(impl->params, "DestinationPort", 49000, 1, 65535);
    next.threshold_db         = param_int(impl->params, "ThresholdDb", -45, -90, 0);
    next.margin_db            = param_int(impl->params, "MarginDb", 6, 0, 40);
    next.activation_delay_ms  = param_int(impl->params, "ActivationDelayMs", 200, 0, 10000);
    next.deactivation_delay_s = param_int(impl->params, "DeactivationDelayS", 2, 0, 60);
    next.max_on_s             = param_int(impl->params, "MaxOnS", 0, 0, 3600);
    param_str(impl->params, "DestinationIp", next.destination_ip, sizeof next.destination_ip, "");
    param_str(impl->params, "InputNode", next.input_node, sizeof next.input_node, DEFAULT_NODE);
    if (!valid_node_name(next.input_node)) {
        snprintf(next.input_node, sizeof next.input_node, "%s", DEFAULT_NODE);
    }
    if (next.destination_ip[0] != '\0' && !valid_ipv4(next.destination_ip)) {
        set_error("Destination IP is not a unicast IPv4 address");
        next.destination_ip[0] = '\0';
    }
    g_cfg = next;
}

static bool set_param(AXParameter* handle, const char* name, const char* value) {
    GError* error = NULL;

    if (handle == NULL) {
        return false;
    }
    if (!ax_parameter_set(handle, name, value, TRUE, &error)) {
        syslog(LOG_ERR, "Failed to set %s: %s", name, error != NULL ? error->message : "unknown");
        if (error != NULL) {
            snprintf(g_last_error, sizeof g_last_error, "Could not save %s", name);
        }
        g_clear_error(&error);
        return false;
    }
    return true;
}

static bool apply_pending(struct impl* impl) {
    struct cfg cfg;
    bool have = false;
    bool have_alarm = false;
    int alarm_level = -1;
    char buf[32];

    pthread_mutex_lock(&g_mu);
    if (g_pending.have) {
        cfg            = g_pending.cfg;
        g_pending.have = false;
        have           = true;
    }
    if (g_pending.have_alarm) {
        alarm_level          = g_pending.alarm_level;
        g_pending.have_alarm = false;
        have_alarm           = true;
    }
    pthread_mutex_unlock(&g_mu);
    if (have_alarm && alarm_level >= 0 && alarm_level <= 100 && alarm_level != g_alarm_level) {
        g_alarm_level = alarm_level;
        if (impl->params != NULL) {
            snprintf(buf, sizeof buf, "%d", alarm_level);
            set_param(impl->params, "CachedAlarmLevel", buf);
        }
        if (strcmp(g_last_error, "Could not read Audio detection level") == 0) {
            g_last_error[0] = '\0';
        }
    }
    if (!have || impl->params == NULL) {
        return have;
    }
    set_param(impl->params, "Enabled", cfg.enabled ? "yes" : "no");
    set_param(impl->params, "ForceOn", cfg.force_on ? "yes" : "no");
    set_param(impl->params, "TestMode", cfg.test_mode ? "yes" : "no");
    set_param(impl->params, "Trigger", cfg.trigger_port == 1 ? "port1" : cfg.trigger_port == 2 ? "port2" : "level");
    set_param(impl->params, "DestinationIp", cfg.destination_ip);
    snprintf(buf, sizeof buf, "%d", cfg.destination_port);
    set_param(impl->params, "DestinationPort", buf);
    snprintf(buf, sizeof buf, "%d", cfg.threshold_db);
    set_param(impl->params, "ThresholdDb", buf);
    snprintf(buf, sizeof buf, "%d", cfg.margin_db);
    set_param(impl->params, "MarginDb", buf);
    snprintf(buf, sizeof buf, "%d", cfg.activation_delay_ms);
    set_param(impl->params, "ActivationDelayMs", buf);
    snprintf(buf, sizeof buf, "%d", cfg.deactivation_delay_s);
    set_param(impl->params, "DeactivationDelayS", buf);
    snprintf(buf, sizeof buf, "%d", cfg.max_on_s);
    set_param(impl->params, "MaxOnS", buf);
    if (valid_node_name(cfg.input_node)) {
        set_param(impl->params, "InputNode", cfg.input_node);
    }
    return true;
}

static void clear_ring(void) {
    g_ring_r     = 0;
    g_ring_w     = 0;
    g_ring_count = 0;
    g_acc        = 0.f;
    g_acc_n      = 0;
}

static void push_pcm(int16_t sample) {
    if (g_ring_count == RING_CAP) {
        g_ring_r = (g_ring_r + 1U) % RING_CAP;
        g_ring_count--;
        g_samples_dropped++;
    }
    g_ring[g_ring_w] = sample;
    g_ring_w         = (g_ring_w + 1U) % RING_CAP;
    g_ring_count++;
}

static void trim_ring(unsigned keep) {
    while (g_ring_count > keep) {
        g_ring_r = (g_ring_r + 1U) % RING_CAP;
        g_ring_count--;
    }
}

static void stop_streaming(const char* reason) {
    if (!g_streaming) {
        return;
    }
    syslog(LOG_INFO, "RTP stopped (%s), level %.1f dBFS, packets %llu", reason, g_level_db,
           (unsigned long long)g_packets_sent);
    g_streaming   = false;
    g_hot_since   = 0;
    g_quiet_since = 0;
    g_marker      = true;
    clear_ring();
}

static void start_streaming(const char* reason) {
    g_streaming    = true;
    g_marker       = true;
    g_quiet_since  = 0;
    g_active_since = now_ms();
    {
        unsigned keep = (unsigned)g_cfg.activation_delay_ms * 8U;
        if (keep < RTP_SAMPLES) {
            keep = RTP_SAMPLES;
        }
        if (keep > RING_CAP) {
            keep = RING_CAP;
        }
        trim_ring(keep);
    }
    syslog(LOG_INFO, "RTP started (%s), level %.1f dBFS, dest %s:%d", reason, g_level_db,
           g_cfg.destination_ip, g_cfg.destination_port);
}

static const char* gate_name(void) {
    if (g_waiting_for_clear) {
        return "waiting";
    }
    if (g_streaming && g_quiet_since != 0) {
        return "holding";
    }
    if (g_streaming && g_cfg.test_mode) {
        return "test";
    }
    if (g_streaming) {
        return "active";
    }
    if (g_hot_since != 0) {
        return "pending";
    }
    return "inactive";
}

static int port_is_active(int which) {
    if (which == 1) {
        if (g_port_numbering == 2) {
            return g_port_active[0];
        }
        return g_port_active[1];
    }
    if (which == 2) {
        if (g_port_numbering == 2) {
            return g_port_active[1];
        }
        return g_port_active[2];
    }
    return -1;
}

static void remember_port(int port, gboolean state) {
    if (port < 0 || port > 2) {
        return;
    }
    if (port == 0) {
        g_port_numbering = 2;
    } else if (g_port_numbering == 0) {
        g_port_numbering = 1;
    }
    g_port_active[port] = state ? 1 : 0;
    syslog(LOG_INFO, "Port %d is %s", port, state ? "active" : "inactive");
}

static void port_event(guint subscription, AXEvent* event, void* user_data) {
    const AXEventKeyValueSet* set = ax_event_get_key_value_set(event);
    gint port = -1;
    gboolean state = FALSE;

    (void)subscription;
    (void)user_data;
    if (ax_event_key_value_set_get_integer(set, "port", NULL, &port, NULL)) {
        ax_event_key_value_set_get_boolean(set, "state", NULL, &state, NULL);
        remember_port(port, state);
    }
    ax_event_free(event);
}

static guint subscribe_ports(AXEventHandler* handler, const char* topic2, const char* topic2_ns) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    guint subscription = 0;
    GError* error = NULL;

    ax_event_key_value_set_add_key_values(set, NULL, "topic0", "tns1", "Device", AX_VALUE_TYPE_STRING, "topic1",
                                          "tnsaxis", "IO", AX_VALUE_TYPE_STRING, "topic2", topic2_ns, topic2,
                                          AX_VALUE_TYPE_STRING, "port", NULL, NULL, AX_VALUE_TYPE_INT, "state", NULL,
                                          NULL, AX_VALUE_TYPE_BOOL, NULL);
    if (!ax_event_handler_subscribe(handler, set, &subscription, (AXSubscriptionCallback)port_event, NULL, &error)) {
        syslog(LOG_ERR, "Port subscription failed: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
    ax_event_key_value_set_free(set);
    return subscription;
}

static void* event_thread(void* unused) {
    AXEventHandler* handler = NULL;
    guint subscription      = 0;
    GMainLoop* loop;

    (void)unused;
    loop = g_main_loop_new(NULL, FALSE);
    if (loop == NULL) {
        g_atomic_int_set(&g_event_ready, -1);
        return NULL;
    }
    g_event_loop = loop;
    handler      = ax_event_handler_new();
    if (handler != NULL) {
        subscription = subscribe_ports(handler, "Port", "tnsaxis");
    }
    g_atomic_int_set(&g_event_ready, 1);
    while (g_atomic_int_get(&g_event_stop) == 0) {
        g_main_context_iteration(g_main_loop_get_context(loop), TRUE);
    }
    if (handler != NULL && subscription != 0) {
        ax_event_handler_unsubscribe(handler, subscription, NULL);
    }
    if (handler != NULL) {
        ax_event_handler_free(handler);
    }
    g_main_loop_unref(loop);
    g_event_loop = NULL;
    return NULL;
}

static void update_gate(void) {
    int64_t now = now_ms();
    double release_percent = (double)g_alarm_level * pow(10.0, -(double)g_cfg.margin_db / 20.0);
    bool have_dest = g_cfg.enabled && g_cfg.destination_ip[0] != '\0';

    if (!have_dest) {
        g_waiting_for_clear = false;
        stop_streaming(g_cfg.enabled ? "no destination" : "disabled");
        return;
    }
    if (g_cfg.test_mode) {
        g_waiting_for_clear = false;
        if (!g_streaming) {
            start_streaming("test tone");
        }
        return;
    }
    if (!g_impl.format_ready) {
        stop_streaming("no capture");
        return;
    }
    if (g_cfg.force_on) {
        g_waiting_for_clear = false;
        if (!g_streaming) {
            start_streaming("force on");
        }
        return;
    }
    bool hot = false;
    bool idle = false;
    if (g_cfg.trigger_port > 0) {
        int active = port_is_active(g_cfg.trigger_port);
        if (active < 0) {
            stop_streaming("input state unknown");
            return;
        }
        hot  = active == 1;
        idle = !hot;
    } else {
        if (g_alarm_level < 0) {
            stop_streaming("audio detection level unavailable");
            return;
        }
        hot  = g_level_percent >= (double)g_alarm_level;
        idle = g_level_percent < release_percent;
    }
    if (g_waiting_for_clear) {
        stop_streaming("max on time");
        if (idle) {
            g_waiting_for_clear = false;
            syslog(LOG_INFO, "Gate re-armed after max-on quiet period");
        }
        return;
    }
    if (!g_streaming) {
        if (hot) {
            if (g_hot_since == 0) {
                g_hot_since = now;
            }
            if (now - g_hot_since >= g_cfg.activation_delay_ms) {
                start_streaming(g_cfg.trigger_port > 0 ? "input" : "level");
            }
        } else {
            g_hot_since = 0;
        }
        return;
    }
    if (g_cfg.max_on_s > 0 && now - g_active_since >= (int64_t)g_cfg.max_on_s * 1000) {
        g_waiting_for_clear = true;
        stop_streaming("max on time");
        return;
    }
    if (idle) {
        if (g_quiet_since == 0) {
            g_quiet_since = now;
        }
        if (now - g_quiet_since >= (int64_t)g_cfg.deactivation_delay_s * 1000) {
            stop_streaming("quiet");
        }
    } else {
        g_quiet_since = 0;
    }
}

static int ensure_socket(void) {
    struct sockaddr_in addr;

    if (g_rtp_fd >= 0 && strcmp(g_rtp_ip, g_cfg.destination_ip) == 0 && g_rtp_port == g_cfg.destination_port) {
        return g_rtp_fd;
    }
    if (g_rtp_fd >= 0) {
        close(g_rtp_fd);
        g_rtp_fd = -1;
    }
    g_rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rtp_fd < 0) {
        set_error("Could not open UDP socket");
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_cfg.destination_port);
    if (inet_pton(AF_INET, g_cfg.destination_ip, &addr.sin_addr) != 1) {
        set_error("Destination IP is not a unicast IPv4 address");
        close(g_rtp_fd);
        g_rtp_fd = -1;
        return -1;
    }
    if (connect(g_rtp_fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
        set_error("Could not connect UDP socket");
        close(g_rtp_fd);
        g_rtp_fd = -1;
        return -1;
    }
    snprintf(g_rtp_ip, sizeof g_rtp_ip, "%s", g_cfg.destination_ip);
    g_rtp_port = g_cfg.destination_port;
    return g_rtp_fd;
}

static void send_packet(void) {
    uint8_t packet[12 + RTP_SAMPLES];
    unsigned i;
    int fd;
    ssize_t sent;

    if (g_ring_count < RTP_SAMPLES) {
        return;
    }
    fd = ensure_socket();
    if (fd < 0) {
        return;
    }
    packet[0] = 0x80;
    packet[1] = (uint8_t)((g_marker ? 0x80 : 0x00) | 8);
    packet[2] = (uint8_t)(g_seq >> 8);
    packet[3] = (uint8_t)(g_seq & 0xFF);
    packet[4] = (uint8_t)(g_timestamp >> 24);
    packet[5] = (uint8_t)(g_timestamp >> 16);
    packet[6] = (uint8_t)(g_timestamp >> 8);
    packet[7] = (uint8_t)(g_timestamp & 0xFF);
    packet[8]  = (uint8_t)(g_ssrc >> 24);
    packet[9]  = (uint8_t)(g_ssrc >> 16);
    packet[10] = (uint8_t)(g_ssrc >> 8);
    packet[11] = (uint8_t)(g_ssrc & 0xFF);
    for (i = 0; i < RTP_SAMPLES; i++) {
        packet[12 + i] = linear_to_alaw(g_ring[g_ring_r]);
        g_ring_r       = (g_ring_r + 1U) % RING_CAP;
        g_ring_count--;
    }
    {
        unsigned i;
        for (i = 0; i < 12; i++) {
            snprintf(g_last_rtp_header + (i * 2), 3, "%02x", packet[i]);
        }
    }
    sent = send(fd, packet, sizeof packet, MSG_DONTWAIT);
    if (sent != (ssize_t)sizeof packet) {
        g_send_errors++;
        if (g_send_errors == 1 || (g_send_errors % 100) == 0) {
            syslog(LOG_WARNING, "RTP send failed: %s", strerror(errno));
            set_error(strerror(errno));
        }
        return;
    }
    g_marker = false;
    g_seq++;
    g_timestamp += RTP_SAMPLES;
    g_packets_sent++;
    if (g_last_error[0] != '\0' && strstr(g_last_error, "send") != NULL) {
        g_last_error[0] = '\0';
    }
}

/* 1 kHz sine at about -12 dBFS, already at the 8 kHz RTP rate. */
static void push_test_tone(unsigned samples) {
    unsigned i;

    for (i = 0; i < samples; i++) {
        double sample = sin(g_tone_phase) * (0.25 * 32767.0);
        push_pcm((int16_t)lrint(sample));
        g_tone_phase += 2.0 * 3.141592653589793 * 1000.0 / 8000.0;
        if (g_tone_phase > 2.0 * 3.141592653589793) {
            g_tone_phase -= 2.0 * 3.141592653589793;
        }
    }
    g_level_db = -12.0;
    g_level_percent = 25.0;
}

static void drain_audio(void) {
    unsigned sent = 0;

    if (!g_streaming) {
        if (g_hot_since == 0) {
            clear_ring();
        }
        return;
    }
    while (g_ring_count > 800U) {
        g_ring_r = (g_ring_r + 1U) % RING_CAP;
        g_ring_count--;
        g_samples_dropped++;
    }
    while (g_ring_count >= RTP_SAMPLES && sent < 4U) {
        send_packet();
        sent++;
    }
}

static void publish_status(const char* gate) {
    struct status_snap snap;

    memset(&snap, 0, sizeof snap);
    snap.level_db             = g_level_db;
    snap.streaming            = g_streaming;
    snap.enabled              = g_cfg.enabled;
    snap.force_on             = g_cfg.force_on;
    snap.test_mode            = g_cfg.test_mode;
    snap.trigger_port         = g_cfg.trigger_port;
    snap.port1_active         = port_is_active(1);
    snap.port2_active         = port_is_active(2);
    snap.node_connected       = g_impl.format_ready;
    snap.threshold_db         = g_cfg.threshold_db;
    snap.margin_db            = g_cfg.margin_db;
    snap.activation_delay_ms  = g_cfg.activation_delay_ms;
    snap.deactivation_delay_s = g_cfg.deactivation_delay_s;
    snap.max_on_s             = g_cfg.max_on_s;
    snap.destination_port     = g_cfg.destination_port;
    snap.device_alarm_level   = g_alarm_level;
    snap.level_percent        = g_level_percent;
    snap.capture_rate         = g_impl.format_ready ? (int)g_impl.info.info.raw.rate : 0;
    snap.capture_channels     = g_impl.format_ready ? (int)g_impl.info.info.raw.channels : 0;
    snap.packets_sent         = g_packets_sent;
    snap.send_errors          = g_send_errors;
    snap.samples_dropped      = g_samples_dropped;
    snap.buffered_samples     = g_ring_count;
    snprintf(snap.gate, sizeof snap.gate, "%s", gate);
    snprintf(snap.destination_ip, sizeof snap.destination_ip, "%s", g_cfg.destination_ip);
    if (g_cfg.destination_ip[0] != '\0') {
        snprintf(snap.stream_url, sizeof snap.stream_url, "rtp://%s:%d", g_cfg.destination_ip, g_cfg.destination_port);
    }
    snprintf(snap.input_node, sizeof snap.input_node, "%s", g_cfg.input_node);
    snprintf(snap.connected_node, sizeof snap.connected_node, "%s", g_impl.connected_name);
    snprintf(snap.last_error, sizeof snap.last_error, "%s", g_last_error);
    snprintf(snap.last_rtp_header, sizeof snap.last_rtp_header, "%s", g_last_rtp_header);

    pthread_mutex_lock(&g_mu);
    g_status = snap;
    pthread_mutex_unlock(&g_mu);
}

static void on_timeout(void* data, uint64_t expirations) {
    struct impl* impl = data;
    int64_t now;

    (void)expirations;
    now = now_ms();
    if (apply_pending(impl) || now - g_last_cfg_ms >= 1000) {
        load_cfg(impl);
        g_last_cfg_ms = now;
    }
    ensure_stream(impl);
    update_gate();
    if (g_cfg.test_mode && g_streaming) {
        push_test_tone(80);
    }
    drain_audio();
    publish_status(gate_name());

    if (g_streaming && now - g_last_report_ms >= 10000) {
        g_last_report_ms = now;
        syslog(LOG_INFO, "Streaming level %.1f dBFS packets %llu errors %llu", g_level_db,
               (unsigned long long)g_packets_sent, (unsigned long long)g_send_errors);
    }
}

static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
    struct impl* impl = data;
    int res;

    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }
    res = spa_format_parse(param, &impl->info.media_type, &impl->info.media_subtype);
    if (res < 0) {
        set_error("Could not parse capture format");
        return;
    }
    if (impl->info.media_type != SPA_MEDIA_TYPE_audio || impl->info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        set_error("Capture format is not raw audio");
        return;
    }
    spa_format_audio_raw_parse(param, &impl->info.info.raw);
    impl->format_ready = true;
    if (impl->info.info.raw.rate >= 8000 && (impl->info.info.raw.rate % 8000) == 0) {
        g_decim = (int)(impl->info.info.raw.rate / 8000);
    } else {
        g_decim = 0;
        set_error("Capture rate is not a multiple of 8000 Hz");
    }
    syslog(LOG_INFO, "Capturing %s, %u ch, %u Hz, format %d, decimation %d", impl->connected_name,
           impl->info.info.raw.channels, impl->info.info.raw.rate, impl->info.info.raw.format, g_decim);
}

static void on_state_changed(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    struct impl* impl = data;

    syslog(LOG_INFO, "PipeWire %s -> %s", pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR) {
        syslog(LOG_ERR, "PipeWire stream error: %s", error != NULL ? error : "");
        set_error("PipeWire stream error");
        impl->format_ready = false;
    }
}

static void consume_samples(const float* left, const float* right, uint32_t count) {
    double sumsq = 0.0;
    float peak = 0.f;
    uint32_t i;

    if (count == 0 || left == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        float sample = left[i];
        float amp = fabsf(left[i]);
        if (right != NULL) {
            amp = fmaxf(amp, fabsf(right[i]));
            sample = 0.5f * (sample + right[i]);
        }
        if (amp > peak) {
            peak = amp;
        }
        sumsq += (double)sample * (double)sample;
        if (g_decim > 0) {
            g_acc += sample;
            g_acc_n++;
            if (g_acc_n >= g_decim) {
                float mixed = g_acc / (float)g_decim;
                if (mixed > 1.f) {
                    mixed = 1.f;
                } else if (mixed < -1.f) {
                    mixed = -1.f;
                }
                push_pcm((int16_t)lrintf(mixed * 32767.f));
                g_acc   = 0.f;
                g_acc_n = 0;
            }
        }
    }
    {
        double rms = sqrt(sumsq / (double)count);
        g_level_db = rms < 1e-9 ? -120.0 : 20.0 * log10(rms);
        if (peak > 1.f) {
            peak = 1.f;
        }
        g_level_percent = (double)peak * 100.0;
    }
}

static const float* audio_samples(const struct spa_data* data, uint32_t* frames) {
    uint32_t offset;
    uint32_t size;

    if (frames == NULL) {
        return NULL;
    }
    *frames = 0;
    if (data == NULL || data->data == NULL || data->chunk == NULL) {
        return NULL;
    }
    offset = data->chunk->offset;
    size   = data->chunk->size;
    if (data->maxsize > 0 && (offset > data->maxsize || size > data->maxsize - offset)) {
        return NULL;
    }
    *frames = size / (uint32_t)sizeof(float);
    return (const float*)((const uint8_t*)data->data + offset);
}

static void on_process(void* data) {
    struct impl* impl = data;
    struct pw_buffer* buffer;
    struct spa_buffer* buf;
    uint32_t count = 0;
    const float* left;

    buffer = pw_stream_dequeue_buffer(impl->stream);
    if (buffer == NULL) {
        return;
    }
    if (g_cfg.test_mode) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    buf = buffer->buffer;
    if (buf->n_datas < 1) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    left = audio_samples(&buf->datas[0], &count);
    if (left == NULL || count == 0) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    if (impl->info.info.raw.format == SPA_AUDIO_FORMAT_F32P) {
        const float* right = NULL;
        if (impl->info.info.raw.channels > 1 && buf->n_datas > 1) {
            uint32_t right_count = 0;
            right                = audio_samples(&buf->datas[1], &right_count);
            if (right != NULL && right_count < count) {
                count = right_count;
            }
        }
        consume_samples(left, right, count);
    } else if (impl->info.info.raw.format == SPA_AUDIO_FORMAT_F32) {
        uint32_t channels = impl->info.info.raw.channels > 0 ? impl->info.info.raw.channels : 1;
        uint32_t frames   = count / channels;
        const float* interleaved = left;
        uint32_t i;
        double sumsq = 0.0;
        float peak = 0.f;

        for (i = 0; i < frames; i++) {
            float sample = interleaved[i * channels];
            float amp = fabsf(sample);
            if (channels > 1) {
                amp = fmaxf(amp, fabsf(interleaved[i * channels + 1]));
                sample = 0.5f * (sample + interleaved[i * channels + 1]);
            }
            if (amp > peak) {
                peak = amp;
            }
            sumsq += (double)sample * (double)sample;
            if (g_decim > 0) {
                g_acc += sample;
                g_acc_n++;
                if (g_acc_n >= g_decim) {
                    float mixed = g_acc / (float)g_decim;
                    if (mixed > 1.f) {
                        mixed = 1.f;
                    } else if (mixed < -1.f) {
                        mixed = -1.f;
                    }
                    push_pcm((int16_t)lrintf(mixed * 32767.f));
                    g_acc   = 0.f;
                    g_acc_n = 0;
                }
            }
        }
        if (frames > 0) {
            double rms = sqrt(sumsq / (double)frames);
            g_level_db = rms < 1e-9 ? -120.0 : 20.0 * log10(rms);
            if (peak > 1.f) {
                peak = 1.f;
            }
            g_level_percent = (double)peak * 100.0;
        }
    } else if (!g_logged_bad_format) {
        g_logged_bad_format = true;
        syslog(LOG_ERR, "Unsupported PipeWire format %d", impl->info.info.raw.format);
        set_error("Unsupported capture format");
    }
    pw_stream_queue_buffer(impl->stream, buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .state_changed = on_state_changed,
};

static void destroy_stream(struct impl* impl) {
    if (impl->stream == NULL) {
        return;
    }
    spa_hook_remove(&impl->stream_listener);
    pw_stream_destroy(impl->stream);
    impl->stream         = NULL;
    impl->format_ready   = false;
    impl->connected_id   = 0;
    impl->connected_name[0] = '\0';
    g_decim              = 0;
    stop_streaming("capture stopped");
    clear_ring();
}

static void connect_node(struct impl* impl, uint32_t id, const char* name) {
    struct pw_properties* props;
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod* params[1];
    int res;

    if (impl->stream != NULL && impl->connected_id == id) {
        return;
    }
    destroy_stream(impl);
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_TARGET_OBJECT,
                              name, NULL);
    if (props == NULL) {
        set_error("Could not create PipeWire properties");
        return;
    }
    impl->stream = pw_stream_new(impl->core, "lineinrtp", props);
    if (impl->stream == NULL) {
        set_error("Could not create PipeWire stream");
        return;
    }
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &stream_events, impl);
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32P));
    res = pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if (res < 0) {
        syslog(LOG_ERR, "PipeWire connect failed for %s: %s", name, strerror(-res));
        set_error("PipeWire connect failed");
        destroy_stream(impl);
        return;
    }
    impl->connected_id = id;
    snprintf(impl->connected_name, sizeof impl->connected_name, "%s", name);
    syslog(LOG_INFO, "Connecting to input node %s id %u", name, id);
}

static void remember_node(struct impl* impl, uint32_t id, const char* name) {
    unsigned i;
    unsigned free_slot = NODE_MAX;

    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && impl->nodes[i].id == id) {
            snprintf(impl->nodes[i].name, sizeof impl->nodes[i].name, "%s", name);
            return;
        }
        if (!impl->nodes[i].used && free_slot == NODE_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == NODE_MAX) {
        return;
    }
    impl->nodes[free_slot].used = true;
    impl->nodes[free_slot].id   = id;
    snprintf(impl->nodes[free_slot].name, sizeof impl->nodes[free_slot].name, "%s", name);
}

static void ensure_stream(struct impl* impl) {
    unsigned i;

    if (impl->stream != NULL && strcmp(impl->connected_name, g_cfg.input_node) == 0) {
        return;
    }
    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && strcmp(impl->nodes[i].name, g_cfg.input_node) == 0) {
            connect_node(impl, impl->nodes[i].id, impl->nodes[i].name);
            return;
        }
    }
    if (impl->stream != NULL) {
        destroy_stream(impl);
    }
}

static void registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                                  const struct spa_dict* props) {
    struct impl* impl = data;
    const char* media_class;
    const char* name;

    (void)permissions;
    (void)version;
    if (!spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        return;
    }
    media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    name        = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == NULL) {
        return;
    }
    syslog(LOG_INFO, "PipeWire node %s class %s id %u", name, media_class != NULL ? media_class : "-", id);
    if (media_class != NULL && strcmp(media_class, "Audio/Source") != 0) {
        return;
    }
    remember_node(impl, id, name);
    if (strcmp(name, g_cfg.input_node) == 0) {
        connect_node(impl, id, name);
    }
}

static void registry_event_global_remove(void* data, uint32_t id) {
    struct impl* impl = data;
    unsigned i;

    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && impl->nodes[i].id == id) {
            impl->nodes[i].used = false;
        }
    }
    if (impl->connected_id == id) {
        syslog(LOG_WARNING, "Input node %s disappeared", impl->connected_name);
        destroy_stream(impl);
        set_error("Input node disappeared");
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void on_signal(void* data, int signal_num) {
    struct impl* impl = data;

    syslog(LOG_INFO, "Caught signal %d", signal_num);
    pw_main_loop_quit(impl->loop);
}

static char* status_json(void) {
    struct status_snap snap;
    json_t* root;
    char* text;

    pthread_mutex_lock(&g_mu);
    snap = g_status;
    pthread_mutex_unlock(&g_mu);

    root = json_object();
    json_object_set_new(root, "levelDb", json_real(snap.level_db));
    json_object_set_new(root, "levelPercent", json_real(snap.level_percent));
    json_object_set_new(root, "deviceAlarmLevel", json_integer(snap.device_alarm_level));
    json_object_set_new(root, "gate", json_string(snap.gate));
    json_object_set_new(root, "streaming", json_boolean(snap.streaming));
    json_object_set_new(root, "enabled", json_boolean(snap.enabled));
    json_object_set_new(root, "forceOn", json_boolean(snap.force_on));
    json_object_set_new(root, "testMode", json_boolean(snap.test_mode));
    json_object_set_new(root, "trigger",
                        json_string(snap.trigger_port == 1 ? "port1" : snap.trigger_port == 2 ? "port2" : "level"));
    json_object_set_new(root, "port1Active", json_integer(snap.port1_active));
    json_object_set_new(root, "port2Active", json_integer(snap.port2_active));
    json_object_set_new(root, "streamUrl", json_string(snap.stream_url));
    json_object_set_new(root, "nodeConnected", json_boolean(snap.node_connected));
    json_object_set_new(root, "thresholdDb", json_integer(snap.threshold_db));
    json_object_set_new(root, "marginDb", json_integer(snap.margin_db));
    json_object_set_new(root, "activationDelayMs", json_integer(snap.activation_delay_ms));
    json_object_set_new(root, "deactivationDelayS", json_integer(snap.deactivation_delay_s));
    json_object_set_new(root, "maxOnS", json_integer(snap.max_on_s));
    json_object_set_new(root, "destinationIp", json_string(snap.destination_ip));
    json_object_set_new(root, "destinationPort", json_integer(snap.destination_port));
    json_object_set_new(root, "inputNode", json_string(snap.input_node));
    json_object_set_new(root, "connectedNode", json_string(snap.connected_node));
    json_object_set_new(root, "captureRate", json_integer(snap.capture_rate));
    json_object_set_new(root, "captureChannels", json_integer(snap.capture_channels));
    json_object_set_new(root, "codec", json_string("PCMA"));
    json_object_set_new(root, "payloadType", json_integer(8));
    json_object_set_new(root, "rtpRate", json_integer(8000));
    json_object_set_new(root, "packetsSent", json_integer((json_int_t)snap.packets_sent));
    json_object_set_new(root, "sendErrors", json_integer((json_int_t)snap.send_errors));
    json_object_set_new(root, "samplesDropped", json_integer((json_int_t)snap.samples_dropped));
    json_object_set_new(root, "bufferedSamples", json_integer(snap.buffered_samples));
    json_object_set_new(root, "lastError", json_string(snap.last_error));
    json_object_set_new(root, "lastRtpHeader", json_string(snap.last_rtp_header));
    text = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return text;
}

static int json_int_field(json_t* root, const char* key, int fallback, int min_v, int max_v, bool* ok, char* err,
                          size_t err_len) {
    json_t* value = json_object_get(root, key);
    json_int_t parsed;

    if (value == NULL) {
        return fallback;
    }
    if (!json_is_integer(value)) {
        snprintf(err, err_len, "%s must be an integer", key);
        *ok = false;
        return fallback;
    }
    parsed = json_integer_value(value);
    if (parsed < min_v || parsed > max_v) {
        snprintf(err, err_len, "%s is out of range", key);
        *ok = false;
        return fallback;
    }
    return (int)parsed;
}

static bool json_bool_field(json_t* root, const char* key, bool fallback, bool* ok, char* err, size_t err_len) {
    json_t* value = json_object_get(root, key);

    if (value == NULL) {
        return fallback;
    }
    if (!json_is_boolean(value)) {
        snprintf(err, err_len, "%s must be a boolean", key);
        *ok = false;
        return fallback;
    }
    return json_is_true(value);
}

static bool json_has_config(json_t* root) {
    static const char* keys[] = {
        "enabled", "forceOn", "testMode", "trigger", "destinationIp", "destinationPort", "thresholdDb",
        "marginDb", "activationDelayMs", "deactivationDelayS", "maxOnS", "inputNode", NULL,
    };
    const char** key;

    for (key = keys; *key != NULL; key++) {
        if (json_object_get(root, *key) != NULL) {
            return true;
        }
    }
    return false;
}

static char* json_ok(bool ok, const char* error) {
    json_t* reply = json_object();
    char* text;

    json_object_set_new(reply, "ok", json_boolean(ok));
    if (error != NULL) {
        json_object_set_new(reply, "error", json_string(error));
    }
    text = json_dumps(reply, JSON_COMPACT);
    json_decref(reply);
    return text;
}

static char* config_from_body(const char* body, size_t length, int* http_code) {
    json_error_t error;
    json_t* root;
    struct status_snap snap;
    struct cfg cfg;
    bool ok = true;
    char err[160];
    json_t* ip_value;
    json_t* node_value;
    json_t* alarm_value = NULL;
    int posted_alarm    = -1;
    bool have_cfg;
    const char* ip;
    const char* node;
    char* text;

    err[0]       = '\0';
    *http_code   = 400;
    root         = json_loadb(body, length, 0, &error);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return json_ok(false, "Body must be a JSON object");
    }
    have_cfg = json_has_config(root);

    pthread_mutex_lock(&g_mu);
    snap = g_status;
    pthread_mutex_unlock(&g_mu);
    memset(&cfg, 0, sizeof cfg);
    cfg.enabled               = snap.enabled;
    cfg.force_on              = snap.force_on;
    cfg.test_mode             = snap.test_mode;
    cfg.trigger_port          = snap.trigger_port;
    cfg.destination_port      = snap.destination_port > 0 ? snap.destination_port : 49000;
    cfg.threshold_db          = snap.threshold_db;
    cfg.margin_db             = snap.margin_db;
    cfg.activation_delay_ms   = snap.activation_delay_ms;
    cfg.deactivation_delay_s  = snap.deactivation_delay_s;
    cfg.max_on_s              = snap.max_on_s;
    snprintf(cfg.destination_ip, sizeof cfg.destination_ip, "%s", snap.destination_ip);
    snprintf(cfg.input_node, sizeof cfg.input_node, "%s", snap.input_node[0] != '\0' ? snap.input_node : DEFAULT_NODE);

    cfg.enabled              = json_bool_field(root, "enabled", cfg.enabled, &ok, err, sizeof err);
    cfg.force_on             = json_bool_field(root, "forceOn", cfg.force_on, &ok, err, sizeof err);
    cfg.test_mode            = json_bool_field(root, "testMode", cfg.test_mode, &ok, err, sizeof err);
    {
        json_t* trigger = json_object_get(root, "trigger");
        if (trigger != NULL) {
            const char* value = json_string_value(trigger);
            if (!json_is_string(trigger) || value == NULL) {
                ok = false;
                snprintf(err, sizeof err, "trigger must be a string");
            } else if (strcmp(value, "port1") == 0) {
                cfg.trigger_port = 1;
            } else if (strcmp(value, "port2") == 0) {
                cfg.trigger_port = 2;
            } else if (strcmp(value, "level") == 0) {
                cfg.trigger_port = 0;
            } else {
                ok = false;
                snprintf(err, sizeof err, "trigger must be level, port1, or port2");
            }
        }
    }
    cfg.destination_port     = json_int_field(root, "destinationPort", cfg.destination_port, 1, 65535, &ok, err, sizeof err);
    cfg.threshold_db         = json_int_field(root, "thresholdDb", cfg.threshold_db, -90, 0, &ok, err, sizeof err);
    cfg.margin_db            = json_int_field(root, "marginDb", cfg.margin_db, 0, 40, &ok, err, sizeof err);
    cfg.activation_delay_ms  = json_int_field(root, "activationDelayMs", cfg.activation_delay_ms, 0, 10000, &ok, err, sizeof err);
    cfg.deactivation_delay_s = json_int_field(root, "deactivationDelayS", cfg.deactivation_delay_s, 0, 60, &ok, err, sizeof err);
    cfg.max_on_s             = json_int_field(root, "maxOnS", cfg.max_on_s, 0, 3600, &ok, err, sizeof err);

    ip_value = json_object_get(root, "destinationIp");
    if (ip_value != NULL) {
        if (!json_is_string(ip_value)) {
            ok = false;
            snprintf(err, sizeof err, "destinationIp must be a string");
        } else {
            ip = json_string_value(ip_value);
            if (ip[0] != '\0' && !valid_ipv4(ip)) {
                ok = false;
                snprintf(err, sizeof err, "destinationIp must be a unicast IPv4 address");
            } else {
                snprintf(cfg.destination_ip, sizeof cfg.destination_ip, "%s", ip);
            }
        }
    }
    alarm_value = json_object_get(root, "deviceAlarmLevel");
    if (alarm_value != NULL) {
        if (!json_is_integer(alarm_value)) {
            ok = false;
            snprintf(err, sizeof err, "deviceAlarmLevel must be an integer");
        } else {
            int level = (int)json_integer_value(alarm_value);
            if (level >= 0 && level <= 100) {
                posted_alarm = level;
            } else {
                ok = false;
                snprintf(err, sizeof err, "deviceAlarmLevel is out of range");
            }
        }
    }
    node_value = json_object_get(root, "inputNode");
    if (node_value != NULL) {
        if (!json_is_string(node_value) || !valid_node_name(json_string_value(node_value))) {
            ok = false;
            snprintf(err, sizeof err, "inputNode has unsupported characters");
        } else {
            node = json_string_value(node_value);
            snprintf(cfg.input_node, sizeof cfg.input_node, "%s", node);
        }
    }
    json_decref(root);

    if (!ok) {
        return json_ok(false, err[0] != '\0' ? err : "Invalid configuration");
    }

    pthread_mutex_lock(&g_mu);
    if (have_cfg) {
        g_pending.cfg  = cfg;
        g_pending.have = true;
    }
    if (posted_alarm >= 0) {
        g_pending.alarm_level = posted_alarm;
        g_pending.have_alarm  = true;
    }
    pthread_mutex_unlock(&g_mu);

    *http_code = 200;
    text       = json_ok(true, NULL);
    return text;
}

static void http_send(int fd, int code, const char* reason, const char* body) {
    char header[256];
    int header_len;

    if (body == NULL) {
        body = "";
    }
    header_len = snprintf(header, sizeof header,
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n",
                          code, reason, strlen(body));
    if (header_len > 0) {
        send(fd, header, (size_t)header_len, MSG_NOSIGNAL);
        send(fd, body, strlen(body), MSG_NOSIGNAL);
    }
}

static const char* header_token(const char* headers, const char* name) {
    size_t name_len = strlen(name);
    const char* line = headers;

    while (line != NULL && line[0] != '\0') {
        const char* next = strstr(line, "\r\n");
        size_t line_len  = next != NULL ? (size_t)(next - line) : strlen(line);

        if (line_len > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            return line + name_len + 1;
        }
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }
    return NULL;
}

static void handle_client(int fd) {
    char request[8192];
    size_t used = 0;
    char* header_end;
    char method[8];
    char path[256];
    const char* length_hdr;
    long content_length = 0;
    char* body;
    struct timeval tv;

    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    while (used + 1 < sizeof request) {
        ssize_t got = recv(fd, request + used, sizeof request - 1 - used, 0);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) {
            break;
        }
    }
    request[used] = '\0';
    header_end    = strstr(request, "\r\n\r\n");
    if (sscanf(request, "%7s %255s", method, path) != 2 || header_end == NULL) {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"Bad request\"}");
        return;
    }
    body          = header_end + 4;
    header_end[0] = '\0';
    length_hdr    = header_token(request, "Content-Length");
    header_end[0] = '\r';
    if (length_hdr != NULL) {
        content_length = strtol(length_hdr, NULL, 10);
        if (content_length < 0 || content_length > 4096) {
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"Body is too large\"}");
            return;
        }
        while ((size_t)(body - request) + (size_t)content_length > used && used + 1 < sizeof request) {
            ssize_t got = recv(fd, request + used, sizeof request - 1 - used, 0);
            if (got <= 0) {
                break;
            }
            used += (size_t)got;
            request[used] = '\0';
        }
        if ((size_t)(body - request) < used) {
            size_t have = used - (size_t)(body - request);
            if ((size_t)content_length > have) {
                content_length = (long)have;
            }
        } else {
            content_length = 0;
        }
    }
    if (strcmp(method, "GET") == 0 && strstr(path, "/api/status") != NULL) {
        char* json = status_json();
        http_send(fd, 200, "OK", json != NULL ? json : "{}");
        free(json);
        return;
    }
    if (strcmp(method, "POST") == 0 && strstr(path, "/api/config") != NULL) {
        int code   = 400;
        char* json = config_from_body(body, (size_t)content_length, &code);
        http_send(fd, code, code == 200 ? "OK" : "Bad Request", json != NULL ? json : "{\"ok\":false}");
        free(json);
        return;
    }
    http_send(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"Not found\"}");
}

static void* http_thread(void* arg) {
    struct impl* impl = arg;

    while (impl->http_listen_fd >= 0) {
        int client = accept(impl->http_listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EBADF || errno == EINVAL) {
                break;
            }
            continue;
        }
        handle_client(client);
        close(client);
    }
    return NULL;
}

static int start_http(struct impl* impl) {
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        syslog(LOG_ERR, "Status server failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    impl->http_listen_fd = fd;
    if (pthread_create(&impl->http_thread, NULL, http_thread, impl) != 0) {
        close(fd);
        impl->http_listen_fd = -1;
        return -1;
    }
    syslog(LOG_INFO, "Status server on 127.0.0.1:%d", HTTP_PORT);
    return 0;
}

static void shutdown_app(struct impl* impl) {
    if (g_event_started) {
        while (g_atomic_int_get(&g_event_ready) == 0) {
            usleep(1000);
        }
        g_atomic_int_set(&g_event_stop, 1);
        if (g_event_loop != NULL) {
            g_main_context_wakeup(g_main_loop_get_context(g_event_loop));
        }
        pthread_join(g_event_thread, NULL);
        g_event_started = false;
    }
    if (impl->http_listen_fd >= 0) {
        int fd               = impl->http_listen_fd;
        impl->http_listen_fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        pthread_join(impl->http_thread, NULL);
    }
    if (g_rtp_fd >= 0) {
        close(g_rtp_fd);
        g_rtp_fd = -1;
    }
    destroy_stream(impl);
    if (g_registry_hooked) {
        spa_hook_remove(&impl->registry_listener);
        g_registry_hooked = false;
    }
    if (impl->registry != NULL) {
        pw_proxy_destroy((struct pw_proxy*)impl->registry);
        impl->registry = NULL;
    }
    if (impl->core != NULL) {
        pw_core_disconnect(impl->core);
        impl->core = NULL;
    }
    if (impl->context != NULL) {
        pw_context_destroy(impl->context);
        impl->context = NULL;
    }
    if (impl->loop != NULL) {
        pw_main_loop_destroy(impl->loop);
        impl->loop = NULL;
    }
    if (g_pw_inited) {
        pw_deinit();
        g_pw_inited = false;
    }
    if (impl->params != NULL) {
        ax_parameter_free(impl->params);
        impl->params = NULL;
    }
    closelog();
}

int main(int argc, char* argv[]) {
    struct impl* impl = &g_impl;
    struct pw_loop* loop;
    struct timespec interval;
    GError* error = NULL;
    int res;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    memset(impl, 0, sizeof *impl);
    impl->http_listen_fd = -1;
    g_seq                = (uint16_t)(now_ms() & 0xFFFF);
    g_timestamp          = (uint32_t)now_ms();
    g_ssrc               = (uint32_t)(now_ms() ^ 0x4C494E45);

    impl->params = ax_parameter_new(APP_NAME, &error);
    if (impl->params == NULL) {
        syslog(LOG_ERR, "AXParameter failed: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
    load_cfg(impl);
    g_last_cfg_ms = now_ms();
    publish_status("inactive");

    {
        int cached = impl->params != NULL ? param_int(impl->params, "CachedAlarmLevel", -1, -1, 100) : -1;
        if (cached >= 0) {
            g_alarm_level = cached;
        }
    }

    setenv("PIPEWIRE_DEBUG", "2", 1);
    pw_init(&argc, &argv);
    g_pw_inited = true;
    impl->loop  = pw_main_loop_new(NULL);
    if (impl->loop == NULL) {
        syslog(LOG_ERR, "Could not create the PipeWire loop");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    loop = pw_main_loop_get_loop(impl->loop);
    pw_loop_add_signal(loop, SIGINT, on_signal, impl);
    pw_loop_add_signal(loop, SIGTERM, on_signal, impl);

    impl->context = pw_context_new(loop, NULL, 0);
    impl->core    = impl->context != NULL ? pw_context_connect(impl->context, NULL, 0) : NULL;
    if (impl->core == NULL) {
        syslog(LOG_ERR, "Could not connect to PipeWire");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
    if (impl->registry == NULL) {
        syslog(LOG_ERR, "Could not open the PipeWire registry");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);
    g_registry_hooked = true;

    impl->timer_source = pw_loop_add_timer(loop, on_timeout, impl);
    interval.tv_sec    = 0;
    interval.tv_nsec   = 10000000L;
    res = impl->timer_source != NULL ? pw_loop_update_timer(loop, impl->timer_source, NULL, &interval, false) : -1;
    if (impl->timer_source == NULL || res < 0) {
        syslog(LOG_ERR, "Could not start the timer%s%s", res < 0 && impl->timer_source != NULL ? ": " : "",
               res < 0 && impl->timer_source != NULL ? strerror(-res) : "");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    if (start_http(impl) != 0) {
        syslog(LOG_ERR, "Continuing without the status server");
    }
    if (pthread_create(&g_event_thread, NULL, event_thread, NULL) == 0) {
        g_event_started = true;
    } else {
        syslog(LOG_ERR, "Input events unavailable");
    }

    syslog(LOG_INFO, "Line Input RTP started. Node %s, threshold %d dBFS, codec PCMA/8000 payload 8",
           g_cfg.input_node, g_cfg.threshold_db);
    pw_main_loop_run(impl->loop);

    syslog(LOG_INFO, "Line Input RTP stopped");
    shutdown_app(impl);
    return EXIT_SUCCESS;
}
