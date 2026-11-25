// SPDX-License-Identifier: GPL-2.0-only
#include "sacn.h"

#include <arpa/inet.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <net/if.h>
#include <pcap/pcap.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CAPTURE_INTERVAL_MS 20
#define CAPTURE_BATCH 128
#define RETRY_INTERVAL_MS 2000

static struct sacn_state state;
static struct blob_buf reply;
static pcap_t *capture;
static const char *interface;
static unsigned ifindex;
static const char *capture_status = "waiting for interface";
static uint64_t batch_time;
static uint64_t capture_drops;
static uint64_t next_interface_check;
static char capture_error[PCAP_ERRBUF_SIZE];
static const char capture_filter[] =
	"(ip and udp dst port 5568) or (vlan and ((ip and udp dst port 5568)"
	" or (vlan and ip and udp dst port 5568)))";

static uint64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void cid_string(const uint8_t *cid, char out[37])
{
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			*out++ = '-';
		*out++ = hex[cid[i] >> 4];
		*out++ = hex[cid[i] & 15];
	}
	*out = '\0';
}

static void source_name(const char input[65], char out[65])
{
	const uint8_t *s = (const uint8_t *)input;
	size_t left = strnlen(input, 64);
	while (left) {
		unsigned c = *s;
		unsigned width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
		bool valid = c >= 0x20 && c != 0x7f && (width == 1 || c >= 0xc2) &&
		    c <= 0xf4 && width <= left;
		for (unsigned i = 1; valid && i < width; i++)
			valid = (s[i] & 0xc0) == 0x80;
		if (valid && width >= 3)
			valid = !(c == 0xe0 && s[1] < 0xa0) && !(c == 0xed && s[1] >= 0xa0) &&
			    !(c == 0xf0 && s[1] < 0x90) && !(c == 0xf4 && s[1] >= 0x90);
		if (!valid) {
			*out++ = '?';
			width = 1;
		} else {
			memcpy(out, s, width);
			out += width;
		}
		s += width;
		left -= width;
	}
	*out = '\0';
}

static int get_stats(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj;
	(void)method;
	(void)msg;

	uint64_t now = monotonic_ms();
	uint32_t sources = 0;
	uint32_t universes = 0;
	struct pcap_stat ps;
	uint64_t drops = capture_drops;
	if (capture && pcap_stats(capture, &ps) == 0)
		drops += ps.ps_drop;

	blob_buf_init(&reply, 0);
	blobmsg_add_string(&reply, "interface", interface);
	blobmsg_add_string(&reply, "capture_status", capture_status);
	if (capture_error[0])
		blobmsg_add_string(&reply, "capture_error", capture_error);
	blobmsg_add_u8(&reply, "capturing", capture != NULL);
	blobmsg_add_u64(&reply, "packets", state.packets);
	blobmsg_add_u64(&reply, "discontinuities", state.discontinuities);
	blobmsg_add_u64(&reply, "untracked_packets", state.untracked);
	blobmsg_add_u64(&reply, "capture_drops", drops);
	blobmsg_add_u32(&reply, "pps", sacn_rate(state.second, state.previous_rate,
					      state.current_rate, now));
	void *array = blobmsg_open_array(&reply, "streams");
	for (size_t i = 0; i < SACN_MAX_STREAMS; i++) {
		const struct sacn_stream *s = &state.streams[i];
		if (!s->used)
			continue;
		bool active = sacn_active(s, now);
		if (active) {
			bool seen_source = false;
			bool seen_universe = false;
			for (size_t j = 0; j < i && !(seen_source && seen_universe); j++) {
				const struct sacn_stream *prev = &state.streams[j];
				if (!sacn_active(prev, now))
					continue;
				seen_source |= !memcmp(prev->packet.cid, s->packet.cid, 16);
				seen_universe |= prev->packet.universe == s->packet.universe;
			}
			sources += !seen_source;
			universes += !seen_universe;
		}
		char cid[37], ip[INET_ADDRSTRLEN], name[65];
		cid_string(s->packet.cid, cid);
		inet_ntop(AF_INET, &s->ip, ip, sizeof(ip));
		source_name(s->packet.name, name);
		void *row = blobmsg_open_table(&reply, NULL);
		blobmsg_add_string(&reply, "cid", cid);
		blobmsg_add_string(&reply, "ip", ip);
		blobmsg_add_string(&reply, "name", name);
		blobmsg_add_u32(&reply, "universe", s->packet.universe);
		blobmsg_add_u32(&reply, "priority", s->packet.priority);
		blobmsg_add_u32(&reply, "pps", active ? sacn_rate(s->second, s->previous_rate,
							      s->current_rate, now) : 0);
		blobmsg_add_u64(&reply, "packets", s->packets);
		blobmsg_add_u64(&reply, "discontinuities", s->discontinuities);
		blobmsg_add_u64(&reply, "age_ms", now - s->last_ms);
		blobmsg_add_u8(&reply, "active", active);
		blobmsg_close_table(&reply, row);
	}
	blobmsg_close_array(&reply, array);
	blobmsg_add_u32(&reply, "active_sources", sources);
	blobmsg_add_u32(&reply, "active_universes", universes);
	ubus_send_reply(ctx, req, reply.head);
	return 0;
}

enum { CHANNELS_CID, CHANNELS_UNIVERSE, CHANNELS_FIELDS };
static const struct blobmsg_policy channel_policy[CHANNELS_FIELDS] = {
	[CHANNELS_CID] = { .name = "cid", .type = BLOBMSG_TYPE_STRING },
	[CHANNELS_UNIVERSE] = { .name = "universe", .type = BLOBMSG_TYPE_INT32 },
};

static int get_channels(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj;
	(void)method;

	struct blob_attr *fields[CHANNELS_FIELDS];
	if (!msg || blobmsg_parse(channel_policy, CHANNELS_FIELDS, fields, blob_data(msg), blob_len(msg)) ||
	    !fields[CHANNELS_CID] || !fields[CHANNELS_UNIVERSE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	const char *cid = blobmsg_get_string(fields[CHANNELS_CID]);
	uint32_t universe = blobmsg_get_u32(fields[CHANNELS_UNIVERSE]);
	if (strlen(cid) != 36 || universe < 1 || universe > 63999)
		return UBUS_STATUS_INVALID_ARGUMENT;

	for (size_t i = 0; i < SACN_MAX_STREAMS; i++) {
		const struct sacn_stream *s = &state.streams[i];
		if (!s->used || s->packet.universe != universe)
			continue;
		char current_cid[37];
		cid_string(s->packet.cid, current_cid);
		if (strcmp(current_cid, cid))
			continue;

		static const char hex[] = "0123456789abcdef";
		char levels[SACN_DMX_SLOTS * 2 + 1];
		for (size_t j = 0; j < s->packet.slot_count; j++) {
			levels[j * 2] = hex[s->packet.slots[j] >> 4];
			levels[j * 2 + 1] = hex[s->packet.slots[j] & 15];
		}
		levels[s->packet.slot_count * 2] = '\0';
		uint64_t now = monotonic_ms();
		blob_buf_init(&reply, 0);
		blobmsg_add_string(&reply, "levels", levels);
		blobmsg_add_u8(&reply, "active", sacn_active(s, now));
		blobmsg_add_u64(&reply, "age_ms", now - s->last_ms);
		ubus_send_reply(ctx, req, reply.head);
		return 0;
	}
	return UBUS_STATUS_NOT_FOUND;
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("stats", get_stats),
	UBUS_METHOD("channels", get_channels, channel_policy),
};
static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("sacnmon", methods);
static struct ubus_object object = {
	.name = "sacnmon",
	.type = &object_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};
static struct ubus_auto_conn bus;

static void on_connect(struct ubus_context *ctx)
{
	if (ubus_add_object(ctx, &object))
		fprintf(stderr, "could not register sacnmon ubus object\n");
}

static void packet_received(u_char *user, const struct pcap_pkthdr *header, const u_char *data)
{
	(void)user;
	struct sacn_packet packet;
	uint32_t ip;
	if (sacn_ethernet(data, header->caplen, &packet, &ip))
		sacn_record(&state, &packet, ip, batch_time);
}

static pcap_t *open_capture(void)
{
	char err[PCAP_ERRBUF_SIZE];
	pcap_t *p = pcap_create(interface, err);
	if (!p) {
		snprintf(capture_error, sizeof(capture_error), "%s", err);
		return NULL;
	}

	/* Explicitly leave promiscuous mode off. Only the capture socket is configured. */
	if (pcap_set_snaplen(p, 768) || pcap_set_buffer_size(p, 256 * 1024) ||
	    pcap_set_timeout(p, CAPTURE_INTERVAL_MS) || pcap_set_immediate_mode(p, 1) ||
	    pcap_activate(p) < 0)
		goto fail;
	if (pcap_datalink(p) != DLT_EN10MB) {
		snprintf(capture_error, sizeof(capture_error), "Ethernet link-layer type required");
		pcap_close(p);
		return NULL;
	}
	if (pcap_setnonblock(p, 1, err))
		goto fail;

	struct bpf_program program;
	if (pcap_compile(p, &program, capture_filter, 1, PCAP_NETMASK_UNKNOWN))
		goto fail;
	int result = pcap_setfilter(p, &program);
	pcap_freecode(&program);
	if (result)
		goto fail;
	return p;

fail:
	snprintf(capture_error, sizeof(capture_error), "%s", pcap_geterr(p));
	pcap_close(p);
	return NULL;
}

static void poll_capture(struct uloop_timeout *timer)
{
	if (!capture) {
		capture_error[0] = '\0';
		ifindex = if_nametoindex(interface);
		if (ifindex)
			capture = open_capture();
		capture_status = capture ? "capturing" :
			ifindex ? "capture unavailable" : "waiting for interface";
	}
	if (capture) {
		batch_time = monotonic_ms();
		bool interface_changed = false;
		if (batch_time >= next_interface_check) {
			next_interface_check = batch_time + 1000;
			interface_changed = if_nametoindex(interface) != ifindex;
		}
		if (interface_changed || pcap_dispatch(capture, CAPTURE_BATCH, packet_received, NULL) < 0) {
			struct pcap_stat ps;
			snprintf(capture_error, sizeof(capture_error), "%s",
				 interface_changed ? "interface changed" : pcap_geterr(capture));
			if (pcap_stats(capture, &ps) == 0)
				capture_drops += ps.ps_drop;
			pcap_close(capture);
			capture = NULL;
			capture_status = "capture interrupted";
		}
	}
	uloop_timeout_set(timer, capture ? CAPTURE_INTERVAL_MS : RETRY_INTERVAL_MS);
}

int main(int argc, char **argv)
{
	if (argc != 2 || !argv[1][0] || strlen(argv[1]) >= IF_NAMESIZE ||
	    !strcmp(argv[1], "any")) {
		fprintf(stderr, "usage: sacnmond ETHERNET_INTERFACE\n");
		return 2;
	}
	interface = argv[1];
	if (uloop_init())
		return 1;

	struct uloop_timeout timer = { .cb = poll_capture };
	bus.cb = on_connect;
	ubus_auto_connect(&bus);
	uloop_timeout_set(&timer, 0);
	uloop_run();
	uloop_timeout_cancel(&timer);
	ubus_auto_shutdown(&bus);
	if (capture)
		pcap_close(capture);
	blob_buf_free(&reply);
	uloop_done();
	return 0;
}
