/*
 *  GDB target glue: register pack/unpack, memory read/write,
 *  software-breakpoint table.
 *
 *  Register layout (i386 target, 16 regs * 4 bytes, little-endian):
 *      eax, ecx, edx, ebx, esp, ebp, esi, edi,
 *      eip, eflags,
 *      cs,  ss,  ds,  es,  fs,  gs
 */

#include "dosbox.h"

#if C_GDBSERVER

#include <string>
#include <map>

#include "regs.h"
#include "cpu.h"
#include "mem.h"
#include "paging.h"
#include "gdbserver.h"
#include "gdb_internal.h"

enum { GDB_REG_COUNT = 16, GDB_REG_BYTES = GDB_REG_COUNT * 4 };

static const SegNames seg_for_reg[6] = { cs, ss, ds, es, fs, gs };

static Bit32u gdb_get_reg(int n) {
	switch (n) {
	case 0:  return reg_eax;
	case 1:  return reg_ecx;
	case 2:  return reg_edx;
	case 3:  return reg_ebx;
	case 4:  return reg_esp;
	case 5:  return reg_ebp;
	case 6:  return reg_esi;
	case 7:  return reg_edi;
	case 8:  return (Bit32u)(SegPhys(cs) + reg_eip);  /* linear, matches breakpoint key */
	case 9:  return (Bit32u)reg_flags;
	case 10: case 11: case 12: case 13: case 14: case 15:
		return Segs.val[seg_for_reg[n - 10]];
	default: return 0;
	}
}

static void gdb_set_reg(int n, Bit32u v) {
	switch (n) {
	case 0:  reg_eax = v; break;
	case 1:  reg_ecx = v; break;
	case 2:  reg_edx = v; break;
	case 3:  reg_ebx = v; break;
	case 4:  reg_esp = v; break;
	case 5:  reg_ebp = v; break;
	case 6:  reg_esi = v; break;
	case 7:  reg_edi = v; break;
	case 8:  reg_eip = v - (Bit32u)SegPhys(cs); break;  /* linear -> offset */
	case 9:  reg_flags = v; break;
	case 10: case 11: case 12: case 13: case 14: case 15:
		CPU_SetSegGeneral(seg_for_reg[n - 10], v & 0xffff);
		break;
	}
}

static void hex_u32_le(Bit32u v, std::string& out) {
	Bit8u b[4] = {
		(Bit8u)(v & 0xff),
		(Bit8u)((v >> 8) & 0xff),
		(Bit8u)((v >> 16) & 0xff),
		(Bit8u)((v >> 24) & 0xff)
	};
	GDB_HexBytes(b, 4, out);
}

void GDB_TargetReadAllRegs(std::string& out) {
	out.clear();
	out.reserve(GDB_REG_BYTES * 2);
	for (int i = 0; i < GDB_REG_COUNT; i++) hex_u32_le(gdb_get_reg(i), out);
}

bool GDB_TargetWriteAllRegs(const char* in, size_t in_len) {
	if (in_len < (size_t)(GDB_REG_BYTES * 2)) return false;
	for (int i = 0; i < GDB_REG_COUNT; i++) {
		Bit8u b[4];
		if (GDB_UnhexBytes(in + i * 8, 8, b, 4) != 4) return false;
		Bit32u v = (Bit32u)b[0] | ((Bit32u)b[1] << 8) |
		           ((Bit32u)b[2] << 16) | ((Bit32u)b[3] << 24);
		gdb_set_reg(i, v);
	}
	return true;
}

bool GDB_TargetReadOneReg(int n, std::string& out) {
	if (n < 0 || n >= GDB_REG_COUNT) return false;
	hex_u32_le(gdb_get_reg(n), out);
	return true;
}

bool GDB_TargetWriteOneReg(int n, const char* in, size_t in_len) {
	if (n < 0 || n >= GDB_REG_COUNT) return false;
	if (in_len < 8) return false;
	Bit8u b[4];
	if (GDB_UnhexBytes(in, 8, b, 4) != 4) return false;
	Bit32u v = (Bit32u)b[0] | ((Bit32u)b[1] << 8) |
	           ((Bit32u)b[2] << 16) | ((Bit32u)b[3] << 24);
	gdb_set_reg(n, v);
	return true;
}

bool GDB_TargetReadMem(PhysPt addr, size_t len, std::string& out) {
	out.reserve(out.size() + len * 2);
	for (size_t i = 0; i < len; i++) {
		Bit8u v;
		if (mem_readb_checked(addr + i, &v)) return false;
		GDB_HexBytes(&v, 1, out);
	}
	return true;
}

bool GDB_TargetWriteMem(PhysPt addr, const Bit8u* data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (mem_writeb_checked(addr + i, data[i])) return false;
	}
	return true;
}

static std::map<PhysPt, int> gdb_breakpoints;

void   GDB_TargetSetSwBreak(PhysPt addr)   { gdb_breakpoints[addr] = 1; }
void   GDB_TargetClearSwBreak(PhysPt addr) { gdb_breakpoints.erase(addr); }
bool   GDB_TargetIsBreakpoint(PhysPt addr) { return gdb_breakpoints.count(addr) > 0; }
void   GDB_TargetClearAllBreaks(void)      { gdb_breakpoints.clear(); }
size_t GDB_TargetBreakCount(void)          { return gdb_breakpoints.size(); }

#endif /* C_GDBSERVER */
