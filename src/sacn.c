// SPDX-License-Identifier: GPL-2.0-only
#include "sacn.h"
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static bool pdu(const uint8_t *p, size_t len)
{
	return (be16(p) & 0xf000) == 0x7000 && (be16(p) & 0xfff) == len;
}

bool sacn_parse(const uint8_t *d, size_t len, struct sacn_packet *p)
{
	static const uint8_t identifier[12] = "ASC-E1.17";
	if (len < 126 || len > 638 || be16(d) != 16 || be16(d + 2) ||
	    memcmp(d + 4, identifier, sizeof(identifier)))
		return false;

	if (!pdu(d + 16, len - 16) || memcmp(d + 18, "\0\0\0\4", 4) ||
	    !pdu(d + 38, len - 38) || memcmp(d + 40, "\0\0\0\2", 4) ||
	    !pdu(d + 115, len - 115) || d[117] != 2 || d[118] != 0xa1 ||
	    be16(d + 119) != 0 || be16(d + 121) != 1 ||
	    be16(d + 123) != len - 125 || d[125] != 0)
		return false;

	uint16_t universe = be16(d + 113);
	if (!universe || universe > 63999 || d[108] > 200 || be16(d + 109) > 63999)
		return false;

	memcpy(p->cid, d + 22, sizeof(p->cid));
	/* Keep the full UTF-8 field. The caller repairs malformed byte sequences. */
	memcpy(p->name, d + 44, 64);
	p->name[64] = '\0';
	p->universe = universe;
	p->priority = d[108];
	p->sequence = d[111];
	p->terminated = (d[112] & 0x40) != 0;
	return true;
}

bool sacn_ethernet(const uint8_t *d, size_t len, struct sacn_packet *p, uint32_t *ip)
{
	if (len < 14)
		return false;

	size_t offset = 14;
	uint16_t type = be16(d + 12);
	for (unsigned tags = 0; tags < 2 && (type == 0x8100 || type == 0x88a8); tags++) {
		if (len - offset < 4)
			return false;
		type = be16(d + offset + 2);
		offset += 4;
	}
	if (type != 0x0800 || len - offset < 20)
		return false;

	d += offset;
	len -= offset;
	size_t header = (d[0] & 15) * 4u;
	size_t total = be16(d + 2);
	if ((d[0] >> 4) != 4 || header < 20 || total < header + 8 || total > len ||
	    d[9] != 17 || (be16(d + 6) & 0x3fff))
		return false;

	const uint8_t *udp = d + header;
	size_t udp_len = be16(udp + 4);
	if (be16(udp + 2) != 5568 || udp_len != total - header ||
	    !sacn_parse(udp + 8, udp_len - 8, p))
		return false;

	memcpy(ip, d + 12, sizeof(*ip));
	return true;
}

bool sacn_active(const struct sacn_stream *s, uint64_t now)
{
	return s->used && !s->packet.terminated && now - s->last_ms < SACN_TIMEOUT_MS;
}

static void tick(uint64_t *second, uint32_t *current, uint32_t *previous, uint64_t now)
{
	uint64_t bucket = now / 1000;
	if (*second != bucket) {
		*previous = bucket - *second == 1 ? *current : 0;
		*current = 0;
		*second = bucket;
	}
	++*current;
}

uint32_t sacn_rate(uint64_t second, uint32_t previous, uint32_t current, uint64_t now)
{
	uint64_t gap = now / 1000 - second;
	return gap == 0 ? previous : gap == 1 ? current : 0;
}

void sacn_record(struct sacn_state *state, const struct sacn_packet *p, uint32_t ip, uint64_t now)
{
	++state->packets;
	tick(&state->second, &state->current_rate, &state->previous_rate, now);

	struct sacn_stream *match = NULL;
	struct sacn_stream *available = NULL;
	for (size_t i = 0; i < SACN_MAX_STREAMS; i++) {
		struct sacn_stream *s = &state->streams[i];
		if (s->used && s->packet.universe == p->universe &&
		    !memcmp(s->packet.cid, p->cid, sizeof(p->cid))) {
			match = s;
			break;
		}
		if (!sacn_active(s, now) &&
		    (!available || (available->used && (!s->used || s->last_ms < available->last_ms))))
			available = s;
	}
	if (!match && !available) {
		++state->untracked;
		return;
	}

	struct sacn_stream *s = match ? match : available;
	if (!match)
		memset(s, 0, sizeof(*s));
	if (sacn_active(s, now)) {
		int delta = (int)p->sequence - s->sequence;
		if (delta > 127)
			delta -= 256;
		if (delta < -128)
			delta += 256;
		if (delta != 1) {
			++s->discontinuities;
			++state->discontinuities;
		}
		if (delta > 0 || delta <= -20)
			s->sequence = p->sequence;
	} else {
		s->sequence = p->sequence;
		s->current_rate = 0;
		s->previous_rate = 0;
	}
	s->packet = *p;
	s->ip = ip;
	s->last_ms = now;
	s->used = true;
	++s->packets;
	tick(&s->second, &s->current_rate, &s->previous_rate, now);
}
