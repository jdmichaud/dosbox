/*
 *  GDB stub internal API shared across gdbserver.cpp / gdb_packet.cpp /
 *  gdb_target.cpp.  Not installed; private to src/gdbserver/.
 */

#ifndef DOSBOX_GDB_INTERNAL_H
#define DOSBOX_GDB_INTERNAL_H

#include "dosbox.h"

#if C_GDBSERVER

#include <SDL_net.h>
#include <string>
#include "mem.h"

extern TCPsocket        gdb_client_sock;
extern SDLNet_SocketSet gdb_socket_set;

extern bool gdb_halted;
extern bool gdb_single_step;
extern bool gdb_single_step_skip;
extern bool gdb_pending_break;
extern bool gdb_skip_bp_once;

/* gdb_packet.cpp */
bool   GDB_PacketRecv(std::string& out, int timeout_ms);
bool   GDB_PacketSend(const std::string& body);
bool   GDB_PacketCheckInterrupt(void);

void   GDB_HexBytes(const Bit8u* in, size_t n, std::string& out);
size_t GDB_UnhexBytes(const char* in, size_t in_len, Bit8u* out, size_t out_max);
bool   GDB_HexToU32(const char* p, size_t len, Bit32u& out);
Bit8u  GDB_HexCharVal(char c);

/* gdb_target.cpp */
void   GDB_TargetReadAllRegs(std::string& hex_out);
bool   GDB_TargetWriteAllRegs(const char* hex_in, size_t hex_len);
bool   GDB_TargetReadOneReg(int regnum, std::string& hex_out);
bool   GDB_TargetWriteOneReg(int regnum, const char* hex_in, size_t hex_len);

bool   GDB_TargetReadMem(PhysPt addr, size_t len, std::string& hex_out);
bool   GDB_TargetWriteMem(PhysPt addr, const Bit8u* data, size_t len);

void   GDB_TargetSetSwBreak(PhysPt addr);
void   GDB_TargetClearSwBreak(PhysPt addr);
bool   GDB_TargetIsBreakpoint(PhysPt addr);
void   GDB_TargetClearAllBreaks(void);
size_t GDB_TargetBreakCount(void);

/* gdbserver.cpp */
void GDB_AcceptIfPending(void);

#endif /* C_GDBSERVER */

#endif
