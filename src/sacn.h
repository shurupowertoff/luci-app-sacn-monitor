// SPDX-License-Identifier: GPL-2.0-only
#ifndef SACN_H
#define SACN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SACN_MAX_STREAMS 256
#define SACN_TIMEOUT_MS 2500
#define SACN_DMX_SLOTS 512

struct sacn_packet {
	uint8_t cid[16];
	char name[65];
	uint16_t universe;
	uint8_t priority;
	uint8_t sequence;
	bool terminated;
	uint16_t slot_count;
	uint8_t slots[SACN_DMX_SLOTS];
};

struct sacn_stream {
	struct sacn_packet packet;
	uint32_t ip;
	uint64_t last_ms;
	uint64_t packets;
	uint64_t discontinuities;
	uint64_t second;
	uint32_t current_rate;
	uint32_t previous_rate;
	uint8_t sequence;
	bool used;
};

struct sacn_state {
	struct sacn_stream streams[SACN_MAX_STREAMS];
	uint64_t packets;
	uint64_t discontinuities;
	uint64_t untracked;
	uint64_t second;
	uint32_t current_rate;
	uint32_t previous_rate;
};

bool sacn_parse(const uint8_t *data, size_t len, struct sacn_packet *packet);
bool sacn_ethernet(const uint8_t *data, size_t len, struct sacn_packet *packet, uint32_t *ip);
bool sacn_active(const struct sacn_stream *stream, uint64_t now);
uint32_t sacn_rate(uint64_t second, uint32_t previous, uint32_t current, uint64_t now);
void sacn_record(struct sacn_state *state, const struct sacn_packet *packet, uint32_t ip, uint64_t now);

#endif
