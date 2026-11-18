/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * Pure logic shared by the ACAP and the host unit tests: the G.711 A-law
 * encoder, input validation and the gate state machine. Nothing here touches
 * PipeWire, AXIS SDK libraries, sockets, clocks or globals, so it builds and
 * runs on any host with a C compiler.
 */
#ifndef RTPCORE_H
#define RTPCORE_H

#include <stdbool.h>
#include <stdint.h>

#define NAME_MAX_LEN 63
#define FLOOR_DB -90 /* lowest threshold; silence on the C8110 sits near -90 dBFS */

/* ITU-T G.711 A-law, 16-bit linear PCM to 8-bit. */
uint8_t linear_to_alaw(int16_t pcm);

/* Canonical dotted IPv4, and a unicast target. */
bool valid_ipv4(const char* text);

/* PipeWire node name: 1..NAME_MAX_LEN of [A-Za-z0-9._-]. */
bool valid_node_name(const char* text);

/* Linear peak 0-100 to dBFS. Zero and below map to -120 dBFS. */
double percent_to_db(double percent);

/* The trigger gate. Times are milliseconds on a monotonic clock; 0 means "not set". */
struct gate {
    bool streaming;
    bool waiting_for_clear; /* max-on tripped; stays closed until the trigger goes idle */
    int64_t hot_since;
    int64_t quiet_since;
    int64_t active_since;
};

struct gate_input {
    bool enabled;
    bool have_destination;
    bool test_mode;
    bool force_on;
    bool format_ready;
    int trigger_port;     /* 0 = audio level, 1 or 2 = I/O port */
    int port_active;      /* for a port trigger: 1 active, 0 inactive, -1 unknown */
    double level_percent; /* linear peak 0-100 */
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
};

enum gate_action { GATE_NONE, GATE_START, GATE_STOP };

struct gate_result {
    enum gate_action action;
    const char* reason; /* set for GATE_START and GATE_STOP */
    bool rearmed;       /* the max-on lockout cleared because the trigger went idle */
};

/* Advance the gate one tick. The gate's own fields are already updated when
 * this returns; the caller only carries out the side effects of the action. */
struct gate_result gate_step(struct gate* g, const struct gate_input* in, int64_t now);

/* Open the gate now. */
void gate_start(struct gate* g, int64_t now);

/* Close the gate. Returns true if it was open. */
bool gate_stop(struct gate* g);

/* Name shown in /api/status: waiting, holding, test, active, pending or inactive. */
const char* gate_state(const struct gate* g, bool test_mode);

#endif
