/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 */
#include "rtpcore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint8_t linear_to_alaw(int16_t pcm) {
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

/* Rejects leading zeros, 0.0.0.0/8, multicast 224.0.0.0/4, and the reserved
 * 240.0.0.0/4 block. */
bool valid_ipv4(const char* text) {
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

bool valid_node_name(const char* text) {
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

double percent_to_db(double percent) {
    return percent > 0.0 ? 20.0 * log10(percent / 100.0) : -120.0;
}

void gate_start(struct gate* g, int64_t now) {
    g->streaming    = true;
    g->quiet_since  = 0;
    g->active_since = now;
}

bool gate_stop(struct gate* g) {
    if (!g->streaming) {
        return false;
    }
    g->streaming   = false;
    g->hot_since   = 0;
    g->quiet_since = 0;
    return true;
}

static struct gate_result result(enum gate_action action, const char* reason) {
    struct gate_result r;

    r.action  = action;
    r.reason  = reason;
    r.rearmed = false;
    return r;
}

static struct gate_result stop(struct gate* g, const char* reason) {
    return gate_stop(g) ? result(GATE_STOP, reason) : result(GATE_NONE, NULL);
}

static struct gate_result start(struct gate* g, int64_t now, const char* reason) {
    if (g->streaming) {
        return result(GATE_NONE, NULL);
    }
    gate_start(g, now);
    return result(GATE_START, reason);
}

struct gate_result gate_step(struct gate* g, const struct gate_input* in, int64_t now) {
    bool hot  = false;
    bool idle = false;

    if (!in->enabled || !in->have_destination) {
        g->waiting_for_clear = false;
        return stop(g, in->enabled ? "no destination" : "disabled");
    }
    if (in->test_mode) {
        g->waiting_for_clear = false;
        return start(g, now, "test tone");
    }
    if (!in->format_ready) {
        return stop(g, "no capture");
    }
    if (in->force_on) {
        g->waiting_for_clear = false;
        return start(g, now, "force on");
    }
    if (in->trigger_port > 0) {
        if (in->port_active < 0) {
            return stop(g, "input state unknown");
        }
        hot  = in->port_active == 1;
        idle = !hot;
    } else {
        /* Threshold and release are dBFS peaks. The release level never goes
         * below the -90 dBFS floor, so a large margin still lets the gate close. */
        double peak_db    = percent_to_db(in->level_percent);
        double release_db = (double)(in->threshold_db - in->margin_db);
        if (release_db < (double)FLOOR_DB) {
            release_db = (double)FLOOR_DB;
        }
        hot  = peak_db >= (double)in->threshold_db;
        idle = peak_db < release_db;
    }
    if (g->waiting_for_clear) {
        struct gate_result r = stop(g, "max on time");
        if (idle) {
            g->waiting_for_clear = false;
            r.rearmed            = true;
        }
        return r;
    }
    if (!g->streaming) {
        if (hot) {
            if (g->hot_since == 0) {
                g->hot_since = now;
            }
            if (now - g->hot_since >= in->activation_delay_ms) {
                return start(g, now, in->trigger_port > 0 ? "input" : "level");
            }
        } else {
            g->hot_since = 0;
        }
        return result(GATE_NONE, NULL);
    }
    if (in->max_on_s > 0 && now - g->active_since >= (int64_t)in->max_on_s * 1000) {
        g->waiting_for_clear = true;
        return stop(g, "max on time");
    }
    if (idle) {
        if (g->quiet_since == 0) {
            g->quiet_since = now;
        }
        if (now - g->quiet_since >= (int64_t)in->deactivation_delay_s * 1000) {
            return stop(g, "quiet");
        }
    } else {
        g->quiet_since = 0;
    }
    return result(GATE_NONE, NULL);
}

const char* gate_state(const struct gate* g, bool test_mode) {
    if (g->waiting_for_clear) {
        return "waiting";
    }
    if (g->streaming && g->quiet_since != 0) {
        return "holding";
    }
    if (g->streaming && test_mode) {
        return "test";
    }
    if (g->streaming) {
        return "active";
    }
    if (g->hot_since != 0) {
        return "pending";
    }
    return "inactive";
}
