/*
 *  GDB remote serial protocol stub for DOSBox.
 *
 *  Lifecycle: TCP listener, GDB_Loop (active while halted),
 *  GDB_HeavyCheck (per-instruction hook), packet dispatch.
 */

#include "dosbox.h"

#if C_GDBSERVER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <SDL.h>
#include <SDL_net.h>

#include "control.h"
#include "setup.h"
#include "cpu.h"
#include "regs.h"
#include "mem.h"
#include "callback.h"
#include "debug.h"
#include "gdbserver.h"
#include "gdb_internal.h"

extern bool exitLoop;
extern void GFX_Events(void);

TCPsocket        gdb_client_sock = NULL;
SDLNet_SocketSet gdb_socket_set  = NULL;

bool gdb_halted             = false;
bool gdb_single_step        = false;
bool gdb_single_step_skip   = false;
bool gdb_pending_break      = false;
bool gdb_skip_bp_once       = false;

static bool         gdb_enabled            = false;
static Bit16u       gdb_port               = 1234;
static bool         gdb_wait_for_connection= false;
static TCPsocket    gdb_listen_sock        = NULL;

static CPU_Decoder* gdb_saved_decoder      = NULL;
static bool         gdb_decoder_swapped    = false;

static bool         gdb_have_pending_stop  = false;
static std::string  gdb_pending_stop_reply;
static bool         gdb_resume_pending     = false;
static std::string  gdb_last_stop_reason   = "S05";


static bool gdb_is_dynamic_core(CPU_Decoder* d) {
	if (d == &CPU_Core_Dynrec_Run || d == &CPU_Core_Dynrec_Trap_Run) return true;
#if (C_DYNAMIC_X86)
	if (d == &CPU_Core_Dyn_X86_Run || d == &CPU_Core_Dyn_X86_Trap_Run) return true;
#endif
	return false;
}

/* Force interpretive core when breakpoints are armed (or single-stepping) so
 * dynrec block boundaries don't hide instruction-level halts. Restore on the
 * way out. */
static void gdb_apply_core_policy(void) {
	bool need_normal = gdb_single_step || GDB_TargetBreakCount() > 0;
	if (need_normal) {
		if (!gdb_decoder_swapped && gdb_is_dynamic_core(cpudecoder)) {
			gdb_saved_decoder   = cpudecoder;
			cpudecoder          = &CPU_Core_Normal_Run;
			gdb_decoder_swapped = true;
		}
	} else if (gdb_decoder_swapped) {
		cpudecoder          = gdb_saved_decoder;
		gdb_saved_decoder   = NULL;
		gdb_decoder_swapped = false;
	}
}

static Bitu GDB_Loop(void);

static void gdb_halt(const char* reply) {
	const char* r = reply ? reply : "S05";
	gdb_last_stop_reason = r;
	if (gdb_resume_pending) {
		gdb_pending_stop_reply = r;
		gdb_have_pending_stop  = true;
		gdb_resume_pending     = false;
	}
	gdb_halted    = true;
	exitLoop      = true;
	CPU_Cycles    = 0;
	CPU_CycleLeft = 0;
	DOSBOX_SetLoop(&GDB_Loop);
}

static void gdb_resume(void) {
	gdb_have_pending_stop = false;
	gdb_halted            = false;
	gdb_resume_pending    = true;
	gdb_apply_core_policy();
	DOSBOX_SetNormalLoop();
}

bool GDB_HeavyCheck(void) {
	if (!gdb_enabled) return false;

	/* Every ~1024 instructions, accept new clients and poll for the Ctrl-C
	 * interrupt byte. A new client always halts immediately so the qSupported /
	 * ? exchange can run inside GDB_Loop. */
	static Bitu poll_counter = 0;
	if ((++poll_counter & 1023) == 0) {
		if (!gdb_client_sock) {
			GDB_AcceptIfPending();
			if (gdb_client_sock) {
				gdb_halt("S05");
				return true;
			}
		}
		if (gdb_client_sock && GDB_PacketCheckInterrupt()) gdb_pending_break = true;
	}

	if (gdb_pending_break) {
		gdb_pending_break = false;
		gdb_halt("S02");                       /* SIGINT */
		return true;
	}

	if (gdb_single_step) {
		if (gdb_single_step_skip) {
			gdb_single_step_skip = false;     /* allow current instruction first */
		} else {
			gdb_single_step = false;
			gdb_halt("S05");                   /* SIGTRAP after one instruction */
			return true;
		}
	}

	if (GDB_TargetBreakCount() > 0) {
		PhysPt cur = SegPhys(cs) + reg_eip;
		if (GDB_TargetIsBreakpoint(cur) && !gdb_skip_bp_once) {
			gdb_halt("S05");
			return true;
		}
		gdb_skip_bp_once = false;
	}

	return false;
}

void GDB_AcceptIfPending(void) {
	if (!gdb_listen_sock || !gdb_socket_set || gdb_client_sock) return;
	gdb_client_sock = SDLNet_TCP_Accept(gdb_listen_sock);
	if (gdb_client_sock) {
		SDLNet_TCP_AddSocket(gdb_socket_set, gdb_client_sock);
		LOG_MSG("GDB: client connected");
	}
}

/* ---- packet dispatch ---------------------------------------------------- */

static const std::string ok_str  = "OK";
static const std::string emp_str = "";

static void gdb_send(const std::string& s) {
	GDB_PacketSend(s);
}

/* Parse "ADDR,LEN" → addr,len in hex. Returns false on parse error. */
static bool parse_addr_len(const char* p, size_t plen, Bit32u& addr, Bit32u& len) {
	const char* comma = (const char*)memchr(p, ',', plen);
	if (!comma) return false;
	if (!GDB_HexToU32(p, comma - p, addr)) return false;
	const char* rest = comma + 1;
	size_t rest_len = plen - (rest - p);
	const char* colon = (const char*)memchr(rest, ':', rest_len);
	size_t len_chars = colon ? (size_t)(colon - rest) : rest_len;
	return GDB_HexToU32(rest, len_chars, len);
}

/* Returns true if execution should resume (continue/step). */
static bool dispatch(const std::string& pkt) {
	if (pkt.empty()) { gdb_send(emp_str); return false; }
	const char  cmd = pkt[0];
	const char* arg = pkt.c_str() + 1;
	size_t      alen = pkt.size() - 1;

	switch (cmd) {
	case '?':
		gdb_send(gdb_last_stop_reason);
		return false;

	case 'g': {
		std::string r;
		GDB_TargetReadAllRegs(r);
		gdb_send(r);
		return false;
	}
	case 'G':
		gdb_send(GDB_TargetWriteAllRegs(arg, alen) ? ok_str : std::string("E01"));
		return false;

	case 'p': {
		Bit32u n = 0;
		if (!GDB_HexToU32(arg, alen, n)) { gdb_send("E01"); return false; }
		std::string r;
		if (!GDB_TargetReadOneReg((int)n, r)) { gdb_send("E01"); return false; }
		gdb_send(r);
		return false;
	}
	case 'P': {
		const char* eq = (const char*)memchr(arg, '=', alen);
		if (!eq) { gdb_send("E01"); return false; }
		Bit32u n = 0;
		if (!GDB_HexToU32(arg, eq - arg, n)) { gdb_send("E01"); return false; }
		const char* val = eq + 1;
		size_t      vlen = alen - (val - arg);
		gdb_send(GDB_TargetWriteOneReg((int)n, val, vlen) ? ok_str : std::string("E01"));
		return false;
	}

	case 'm': {
		Bit32u addr, len;
		if (!parse_addr_len(arg, alen, addr, len)) { gdb_send("E01"); return false; }
		std::string r;
		if (!GDB_TargetReadMem(addr, len, r)) { gdb_send("E14"); return false; }
		gdb_send(r);
		return false;
	}
	case 'M': {
		Bit32u addr, len;
		if (!parse_addr_len(arg, alen, addr, len)) { gdb_send("E01"); return false; }
		const char* colon = (const char*)memchr(arg, ':', alen);
		if (!colon) { gdb_send("E01"); return false; }
		const char* hex = colon + 1;
		size_t      hex_len = alen - (hex - arg);
		Bit8u       buf[4096];
		if (len > sizeof(buf)) { gdb_send("E22"); return false; }
		size_t got = GDB_UnhexBytes(hex, hex_len, buf, len);
		if (got != len) { gdb_send("E01"); return false; }
		gdb_send(GDB_TargetWriteMem(addr, buf, len) ? ok_str : std::string("E14"));
		return false;
	}
	case 'X': {
		Bit32u addr, len;
		if (!parse_addr_len(arg, alen, addr, len)) { gdb_send("E01"); return false; }
		const char* colon = (const char*)memchr(arg, ':', alen);
		if (!colon) { gdb_send("E01"); return false; }
		const Bit8u* bin = (const Bit8u*)(colon + 1);
		size_t       bin_len = alen - ((colon + 1) - arg);
		if (bin_len < len) { gdb_send("E01"); return false; }
		gdb_send(GDB_TargetWriteMem(addr, bin, len) ? ok_str : std::string("E14"));
		return false;
	}

	case 'c': {
		if (alen > 0) {
			Bit32u new_lin = 0;
			if (GDB_HexToU32(arg, alen, new_lin)) reg_eip = new_lin - (Bit32u)SegPhys(cs);
		}
		gdb_skip_bp_once = true; /* don't immediately re-trigger if standing on a bp */
		gdb_apply_core_policy();
		return true;
	}
	case 's': {
		if (alen > 0) {
			Bit32u new_lin = 0;
			if (GDB_HexToU32(arg, alen, new_lin)) reg_eip = new_lin - (Bit32u)SegPhys(cs);
		}
		gdb_single_step      = true;
		gdb_single_step_skip = true;
		gdb_skip_bp_once     = true;
		gdb_apply_core_policy();
		return true;
	}

	case 'Z':
	case 'z': {
		if (alen < 1) { gdb_send("E01"); return false; }
		char type = arg[0];
		if (alen < 2 || arg[1] != ',') { gdb_send(emp_str); return false; }
		Bit32u addr, kind;
		if (!parse_addr_len(arg + 2, alen - 2, addr, kind)) { gdb_send("E01"); return false; }
		if (type == '0' || type == '1') {
			if (cmd == 'Z') GDB_TargetSetSwBreak((PhysPt)addr);
			else            GDB_TargetClearSwBreak((PhysPt)addr);
			gdb_apply_core_policy();
			gdb_send(ok_str);
		} else {
			gdb_send(emp_str);  /* watchpoints not supported */
		}
		return false;
	}

	case 'q':
		if (!strncmp(arg, "Supported", 9)) {
			gdb_send("PacketSize=4000;swbreak+");
		} else if (!strncmp(arg, "Attached", 8)) {
			gdb_send("1");
		} else if (alen == 1 && arg[0] == 'C') {
			gdb_send("QC1");
		} else if (!strncmp(arg, "fThreadInfo", 11)) {
			gdb_send("m1");
		} else if (!strncmp(arg, "sThreadInfo", 11)) {
			gdb_send("l");
		} else if (!strncmp(arg, "TStatus", 7)) {
			gdb_send(emp_str);   /* tracepoints not supported */
		} else if (!strncmp(arg, "Offsets", 7)) {
			gdb_send("Text=0;Data=0;Bss=0");
		} else {
			gdb_send(emp_str);
		}
		return false;

	case 'H':
		gdb_send(ok_str);  /* single thread; any thread id is fine */
		return false;

	case 'T':
		gdb_send(ok_str);  /* thread alive */
		return false;

	case 'D':
		gdb_send(ok_str);
		if (gdb_client_sock) {
			SDLNet_TCP_DelSocket(gdb_socket_set, gdb_client_sock);
			SDLNet_TCP_Close(gdb_client_sock);
			gdb_client_sock = NULL;
		}
		GDB_TargetClearAllBreaks();
		gdb_single_step = false;
		gdb_apply_core_policy();
		return true;  /* resume execution */

	case 'k':
		LOG_MSG("GDB: client requested kill");
		throw 0;       /* same mechanism the curses debugger uses to exit DOSBox */

	default:
		gdb_send(emp_str);
		return false;
	}
}

static Bitu GDB_Loop(void) {
	GFX_Events();

	if (!gdb_client_sock) {
		GDB_AcceptIfPending();
		if (!gdb_client_sock) {
			SDL_Delay(20);
			return 0;
		}
	}

	if (gdb_have_pending_stop) {
		GDB_PacketSend(gdb_pending_stop_reply);
		gdb_have_pending_stop = false;
	}

	std::string pkt;
	if (GDB_PacketRecv(pkt, 50)) {
		bool resume = dispatch(pkt);
		if (resume) {
			gdb_resume();
			return 0;
		}
	} else {
		SDL_Delay(1);
	}
	return 0;
}

/* ---- lifecycle ---------------------------------------------------------- */

void GDB_Init(Section* sec) {
	Section_prop* section = static_cast<Section_prop*>(sec);
	gdb_enabled             = section->Get_bool("enabled");
	gdb_port                = (Bit16u)section->Get_int("port");
	gdb_wait_for_connection = section->Get_bool("wait_for_connection");

	sec->AddDestroyFunction(&GDB_Shutdown, true);

	if (!gdb_enabled) return;

	if (!SDLNetInited) {
		if (SDLNet_Init() == -1) {
			LOG_MSG("GDB: SDLNet_Init failed: %s", SDLNet_GetError());
			gdb_enabled = false;
			return;
		}
		SDLNetInited = true;
	}

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, NULL, gdb_port) == -1) {
		LOG_MSG("GDB: SDLNet_ResolveHost failed: %s", SDLNet_GetError());
		gdb_enabled = false;
		return;
	}
	gdb_listen_sock = SDLNet_TCP_Open(&ip);
	if (!gdb_listen_sock) {
		LOG_MSG("GDB: SDLNet_TCP_Open(port %u) failed: %s", gdb_port, SDLNet_GetError());
		gdb_enabled = false;
		return;
	}
	gdb_socket_set = SDLNet_AllocSocketSet(2);
	if (!gdb_socket_set) {
		LOG_MSG("GDB: SDLNet_AllocSocketSet failed: %s", SDLNet_GetError());
		SDLNet_TCP_Close(gdb_listen_sock);
		gdb_listen_sock = NULL;
		gdb_enabled = false;
		return;
	}
	SDLNet_TCP_AddSocket(gdb_socket_set, gdb_listen_sock);
	LOG_MSG("GDB: listening on port %u", gdb_port);

	if (gdb_wait_for_connection) {
		LOG_MSG("GDB: waiting for client to connect on port %u ...", gdb_port);
		while (!gdb_client_sock) {
			gdb_client_sock = SDLNet_TCP_Accept(gdb_listen_sock);
			if (gdb_client_sock) {
				SDLNet_TCP_AddSocket(gdb_socket_set, gdb_client_sock);
				LOG_MSG("GDB: client connected.");
				break;
			}
			GFX_Events();
			SDL_Delay(10);
		}
		/* Halt before the first guest instruction so gdb sees the entry point. */
		gdb_halt("S05");
	}
}

void GDB_Shutdown(Section* /*sec*/) {
	if (gdb_client_sock) {
		if (gdb_socket_set) SDLNet_TCP_DelSocket(gdb_socket_set, gdb_client_sock);
		SDLNet_TCP_Close(gdb_client_sock);
		gdb_client_sock = NULL;
	}
	if (gdb_listen_sock) {
		if (gdb_socket_set) SDLNet_TCP_DelSocket(gdb_socket_set, gdb_listen_sock);
		SDLNet_TCP_Close(gdb_listen_sock);
		gdb_listen_sock = NULL;
	}
	if (gdb_socket_set) {
		SDLNet_FreeSocketSet(gdb_socket_set);
		gdb_socket_set = NULL;
	}
	GDB_TargetClearAllBreaks();
	if (gdb_decoder_swapped) {
		cpudecoder = gdb_saved_decoder;
		gdb_decoder_swapped = false;
	}
}

void GDB_RequestBreak(void) {
	gdb_pending_break = true;
}

#endif /* C_GDBSERVER */
