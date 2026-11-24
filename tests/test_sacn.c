// SPDX-License-Identifier: GPL-2.0-only
#include "../src/sacn.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *data, unsigned value)
{
	data[0] = value >> 8;
	data[1] = value & 255;
}

static void fixture(uint8_t *data, size_t len)
{
	memset(data, 0, len);
	put16(data, 16);
	memcpy(data + 4, "ASC-E1.17", 9);
	put16(data + 16, 0x7000 | (len - 16));
	data[21] = 4;
	data[22] = 1;
	put16(data + 38, 0x7000 | (len - 38));
	data[43] = 2;
	memcpy(data + 44, "Lighting console", 16);
	data[108] = 100;
	data[111] = 255;
	put16(data + 113, 42);
	put16(data + 115, 0x7000 | (len - 115));
	data[117] = 2;
	data[118] = 0xa1;
	put16(data + 121, 1);
	put16(data + 123, len - 125);
}

static void test_parser(void)
{
	uint8_t data[639];
	struct sacn_packet p;
	fixture(data, 638);
	assert(sacn_parse(data, 638, &p));
	assert(p.cid[0] == 1 && p.universe == 42 && p.priority == 100 && p.sequence == 255);
	assert(!strcmp(p.name, "Lighting console") && !p.terminated);
	for (size_t len = 0; len < 638; len++)
		assert(!sacn_parse(data, len, &p));
	assert(!sacn_parse(data, 639, &p));

	const size_t corrupted[] = {
		1, 2, 4, 16, 17, 21, 38, 39, 43, 115, 116, 117, 118, 120, 122, 124, 125
	};
	for (size_t i = 0; i < sizeof(corrupted) / sizeof(*corrupted); i++) {
		size_t offset = corrupted[i];
		data[offset] ^= 1;
		assert(!sacn_parse(data, 638, &p));
		data[offset] ^= 1;
	}
	data[108] = 201;
	assert(!sacn_parse(data, 638, &p));
	data[108] = 200;
	put16(data + 113, 0);
	assert(!sacn_parse(data, 638, &p));
	put16(data + 113, 64000);
	assert(!sacn_parse(data, 638, &p));
	put16(data + 113, 63999);
	assert(sacn_parse(data, 638, &p));
	fixture(data, 126);
	assert(sacn_parse(data, 126, &p));
	data[112] = 0x40;
	assert(sacn_parse(data, 126, &p) && p.terminated);
}

static void test_frames(void)
{
	struct sacn_packet p;
	uint32_t ip;
	for (unsigned tags = 0; tags <= 2; tags++) {
		uint8_t frame[22 + 24 + 8 + 126] = {0};
		size_t link = 14 + tags * 4;
		for (unsigned i = 0; i < tags; i++)
			put16(frame + 12 + i * 4, i == 0 ? 0x88a8 : 0x8100);
		put16(frame + link - 2, 0x0800);
		uint8_t *ipv4 = frame + link;
		size_t ip_len = 24 + 8 + 126;
		ipv4[0] = 0x46;
		put16(ipv4 + 2, ip_len);
		ipv4[9] = 17;
		memcpy(ipv4 + 12, (uint8_t[]){2, 1, 3, 4}, 4);
		uint8_t *udp = ipv4 + 24;
		put16(udp + 2, 5568);
		put16(udp + 4, 8 + 126);
		fixture(udp + 8, 126);

		assert(sacn_ethernet(frame, link + ip_len, &p, &ip));
		assert(!memcmp(&ip, ipv4 + 12, 4));
		for (size_t len = 0; len < link + ip_len; len++)
			assert(!sacn_ethernet(frame, len, &p, &ip));
		put16(ipv4 + 6, 0x2000);
		assert(!sacn_ethernet(frame, link + ip_len, &p, &ip));
		put16(ipv4 + 6, 0);
		put16(udp + 2, 5569);
		assert(!sacn_ethernet(frame, link + ip_len, &p, &ip));
		put16(udp + 2, 5568);
		put16(udp + 4, 7);
		assert(!sacn_ethernet(frame, link + ip_len, &p, &ip));
	}
}

static void test_streams(void)
{
	struct sacn_state state = {0};
	struct sacn_packet a = { .cid = {1}, .universe = 42, .sequence = 254 };
	struct sacn_packet b = { .cid = {2}, .universe = 42, .sequence = 100 };
	sacn_record(&state, &a, 1, 1000);
	sacn_record(&state, &b, 1, 1001);
	sacn_record(&state, &b, 2, 1002);
	sacn_record(&state, &b, 2, 1003);
	assert(state.streams[1].ip == 2 && state.streams[1].discontinuities == 2);
	a.sequence = 255;
	sacn_record(&state, &a, 1, 1010);
	a.sequence = 0;
	sacn_record(&state, &a, 1, 1020);
	assert(state.streams[0].discontinuities == 0);
	a.sequence = 2;
	sacn_record(&state, &a, 1, 1030);
	a.sequence = 1;
	sacn_record(&state, &a, 1, 1040);
	a.sequence = 3;
	sacn_record(&state, &a, 1, 1050);
	assert(state.streams[0].discontinuities == 2 && state.discontinuities == 4);
	a.universe = 43;
	sacn_record(&state, &a, 1, 1060);
	assert(state.streams[2].used && state.packets == 10);
	assert(sacn_rate(state.second, state.previous_rate, state.current_rate, 2000) == 10);
	assert(sacn_rate(state.second, state.previous_rate, state.current_rate, 3000) == 0);
	assert(sacn_active(&state.streams[0], 3549));
	assert(!sacn_active(&state.streams[0], 3550));
	a.universe = 42;
	a.sequence = 90;
	sacn_record(&state, &a, 1, 3550);
	assert(state.streams[0].discontinuities == 2 && sacn_active(&state.streams[0], 3550));
	a.sequence = 91;
	a.terminated = true;
	sacn_record(&state, &a, 1, 3551);
	assert(!sacn_active(&state.streams[0], 3551));
}

static void test_capacity(void)
{
	struct sacn_state state = {0};
	struct sacn_packet p = { .cid = {1} };
	for (unsigned i = 1; i <= SACN_MAX_STREAMS; i++) {
		p.universe = i;
		sacn_record(&state, &p, 0, 1000);
	}
	p.universe++;
	sacn_record(&state, &p, 0, 1001);
	assert(state.untracked == 1 && state.packets == SACN_MAX_STREAMS + 1);
	assert(state.streams[0].packet.universe == 1);
	sacn_record(&state, &p, 0, 3500);
	assert(state.untracked == 1 && state.streams[0].packets == 1);
	assert(state.streams[0].packet.universe == SACN_MAX_STREAMS + 1);
}

int main(void)
{
	test_parser();
	test_frames();
	test_streams();
	test_capacity();
	puts("PASS: E1.31 parser, Ethernet/VLAN, stream lifecycle and capacity");
	return 0;
}
