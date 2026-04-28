/*
 *  GDB RSP packet framing: $...#xx, checksum, hex helpers.
 */

#include "dosbox.h"

#if C_GDBSERVER

#include <stdio.h>
#include <string.h>
#include <string>
#include <SDL_net.h>

#include "gdbserver.h"
#include "gdb_internal.h"

static const char hex_digits[] = "0123456789abcdef";

Bit8u GDB_HexCharVal(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0xff;
}

void GDB_HexBytes(const Bit8u* in, size_t n, std::string& out) {
	out.reserve(out.size() + n * 2);
	for (size_t i = 0; i < n; i++) {
		out += hex_digits[in[i] >> 4];
		out += hex_digits[in[i] & 0xf];
	}
}

size_t GDB_UnhexBytes(const char* in, size_t in_len, Bit8u* out, size_t out_max) {
	size_t n = in_len / 2;
	if (n > out_max) n = out_max;
	for (size_t i = 0; i < n; i++) {
		Bit8u hi = GDB_HexCharVal(in[i * 2]);
		Bit8u lo = GDB_HexCharVal(in[i * 2 + 1]);
		if (hi == 0xff || lo == 0xff) return i;
		out[i] = (hi << 4) | lo;
	}
	return n;
}

bool GDB_HexToU32(const char* p, size_t len, Bit32u& out) {
	out = 0;
	if (len == 0) return false;
	for (size_t i = 0; i < len; i++) {
		Bit8u v = GDB_HexCharVal(p[i]);
		if (v == 0xff) return false;
		out = (out << 4) | v;
	}
	return true;
}

static bool gdb_read_byte(Bit8u* out) {
	if (!gdb_client_sock) return false;
	int n = SDLNet_TCP_Recv(gdb_client_sock, out, 1);
	return n == 1;
}

static bool gdb_wait_readable(int timeout_ms) {
	if (!gdb_socket_set || !gdb_client_sock) return false;
	int active = SDLNet_CheckSockets(gdb_socket_set, timeout_ms);
	if (active <= 0) return false;
	return SDLNet_SocketReady(gdb_client_sock) != 0;
}

bool GDB_PacketRecv(std::string& out, int timeout_ms) {
	out.clear();
	if (!gdb_client_sock) return false;
	if (!gdb_wait_readable(timeout_ms)) return false;

	Bit8u b;
	/* Sync to '$', allow leading acks and the async 0x03 interrupt byte. */
	while (true) {
		if (!gdb_read_byte(&b)) return false;
		if (b == '$') break;
		if (b == 0x03) gdb_pending_break = true;
		/* '+' / '-' / stray bytes ignored */
	}

	Bit8u csum_calc = 0;
	while (true) {
		if (!gdb_read_byte(&b)) return false;
		if (b == '#') break;
		if (b == '}') {
			csum_calc += b;
			Bit8u esc;
			if (!gdb_read_byte(&esc)) return false;
			csum_calc += esc;
			out += (char)(esc ^ 0x20);
		} else {
			csum_calc += b;
			out += (char)b;
		}
	}

	Bit8u h1, h2;
	if (!gdb_read_byte(&h1)) return false;
	if (!gdb_read_byte(&h2)) return false;
	Bit8u csum_recv = (Bit8u)((GDB_HexCharVal(h1) << 4) | GDB_HexCharVal(h2));

	Bit8u ack = (csum_recv == csum_calc) ? '+' : '-';
	SDLNet_TCP_Send(gdb_client_sock, &ack, 1);

	return ack == '+';
}

bool GDB_PacketSend(const std::string& body) {
	if (!gdb_client_sock) return false;

	std::string framed;
	framed.reserve(body.size() + 4);
	framed += '$';
	Bit8u csum = 0;
	for (size_t i = 0; i < body.size(); i++) {
		Bit8u b = (Bit8u)body[i];
		framed += body[i];
		csum += b;
	}
	framed += '#';
	framed += hex_digits[csum >> 4];
	framed += hex_digits[csum & 0xf];

	for (int attempt = 0; attempt < 2; attempt++) {
		int sent = SDLNet_TCP_Send(gdb_client_sock, framed.c_str(), (int)framed.size());
		if (sent != (int)framed.size()) return false;
		if (!gdb_wait_readable(2000)) return false;
		Bit8u ack;
		if (!gdb_read_byte(&ack)) return false;
		if (ack == '+') return true;
		if (ack != '-') return true; /* No-ack mode or unexpected; assume delivered. */
	}
	return false;
}

bool GDB_PacketCheckInterrupt(void) {
	if (!gdb_client_sock || !gdb_socket_set) return false;
	int active = SDLNet_CheckSockets(gdb_socket_set, 0);
	if (active <= 0) return false;
	if (!SDLNet_SocketReady(gdb_client_sock)) return false;

	Bit8u b;
	int n = SDLNet_TCP_Recv(gdb_client_sock, &b, 1);
	if (n != 1) {
		/* disconnected */
		SDLNet_TCP_DelSocket(gdb_socket_set, gdb_client_sock);
		SDLNet_TCP_Close(gdb_client_sock);
		gdb_client_sock = NULL;
		return false;
	}
	return b == 0x03;
}

#endif /* C_GDBSERVER */
