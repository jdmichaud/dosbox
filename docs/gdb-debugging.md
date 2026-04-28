# Debugging DOS programs with GDB

This DOSBox fork ships an optional **GDB remote-serial-protocol (RSP)
stub** so that an external `gdb` can attach over TCP and debug code
running inside the DOS guest — read/write registers and memory, set
breakpoints, single-step, interrupt long-running code, and so on.

This page is self-contained: prerequisites, how to build, how to enable,
how the gdb-remote mechanism works, and how to actually use it day to
day. If you only want to use the feature, skip to *Quick start*.

## What it is and is not

**Is:**

- A stub that speaks the standard GDB Remote Serial Protocol over TCP,
  so any `gdb` (or compatible client like `lldb` with `gdb-remote`) can
  attach.
- Halts the guest CPU on attach, breakpoint hit, single-step, or
  Ctrl-C from the gdb side.
- Works for both real-mode 16-bit DOS programs and 32-bit DPMI/DOS-extender
  programs, with one caveat (see *Address convention* below).

**Is not:**

- Source-level debugging out of the box. gdb has no symbol file by
  default. You can `add-symbol-file` if you have one — most DOS
  toolchains don't produce DWARF-or-similar that gdb can ingest, but
  raw `.sym` files from Watcom/Borland can sometimes be coerced.
- A replacement for the curses-based built-in debugger when you want
  DOSBox-aware features (BIOS interrupt tracing, instruction logs,
  etc.). Both are available in the same build; use whichever fits.
- Aware of multiple threads — DOS is single-threaded, and the stub
  reports a single fake thread (`thread 1`).
- Faster than the regular emulator. While any breakpoint is armed (or
  while single-stepping), the dynrec JIT is forced down to the
  interpreter for correctness; expect a meaningful slowdown.

## Prerequisites

- **SDL_net 1.2** (the SDL 1.2-era library, *not* SDL2).
  Debian/Ubuntu: `sudo apt install libsdl-net1.2-dev`.
- **A curses library** (ncurses on Linux, pdcurses elsewhere). Required
  by `--enable-debug=heavy`, which the gdb stub depends on for its
  per-instruction CPU hook. Debian/Ubuntu: `sudo apt install libncurses-dev`.
- A working **gdb** that knows the i386 architecture. Stock `gdb` on
  x86/x86_64 hosts is fine; on other hosts use `gdb-multiarch`.

## Build

```sh
./autogen.sh                                              # only if you changed *.am / *.ac
./configure --enable-debug=heavy --enable-gdbserver
make -j$(nproc)
src/dosbox --version                                      # sanity check
```

`./configure` will refuse `--enable-gdbserver` without the heavy
debugger and SDL_net 1.2; install the missing prerequisite if you hit
that.

## How the gdb remote mechanism works

GDB has two roles in mind:

1. **Client (`gdb`)** — the user-facing tool, runs on the developer
   machine. It does not execute the program; it tells the *target* to
   step, read memory, etc., and renders the answers.
2. **Target (the program being debugged)** — execution happens here.
   The target exposes a *remote stub* that speaks the GDB Remote Serial
   Protocol on a transport (typically TCP, sometimes a serial line).

When the user types `target remote host:port`, gdb opens a TCP
connection to the stub and they exchange ASCII text packets framed as
`$<body>#<2-byte-hex-checksum>`. Each side acknowledges with `+` or
asks for retransmit with `-`. Common packet types:

- `?`     — "Why are you stopped?" → stub replies `S<sigtrap>` etc.
- `g`     — "Send all registers." → 16 i386 registers, hex.
- `m A,L` — "Read L bytes at linear address A." → hex bytes.
- `c`     — "Continue execution." → stub resumes; on next halt it sends
            an unsolicited stop reply.
- `s`     — "Step one instruction." → similar.
- `Z0,A,K`/`z0,A,K` — set / clear a software breakpoint at A.
- Plus a small zoo of `q…` capability and threading queries.

In our case the stub *is* DOSBox itself — there is no separate
`gdbserver` binary. DOSBox keeps emulating the guest, and when the stub
decides it should halt (a breakpoint hits, the user sent Ctrl-C, etc.)
it parks the CPU, switches the main loop to a packet-servicing loop,
talks to gdb until it gets `c`/`s`/`D`, then resumes.

## Quick start

Pick one of two activation styles.

### Command-line

```sh
src/dosbox -gdbserver 1234                # listen on :1234, attach later
src/dosbox -gdbserver 1234 -gdbserver-wait   # block before first guest insn until gdb attaches
```

### `dosbox.conf`

Add a `[gdbserver]` section:

```ini
[gdbserver]
enabled=true
port=1234
wait_for_connection=false
```

Either way, once DOSBox is up, attach gdb:

```sh
gdb -ex 'set arch i386' -ex 'target remote localhost:1234'
```

For real-mode 16-bit code you may prefer `set arch i8086` so gdb
disassembles in 16-bit mode. The wire protocol is identical.

## A typical session

Assume DOSBox is running with `-gdbserver 1234` and you've attached:

```
(gdb) info reg                                # current state
eax            0x10                10
…
eip            0xf120c             988684
cs             0xf000              61440

(gdb) x/4i $eip                               # disassemble four insns from PC

(gdb) si                                      # step one instruction

(gdb) b *0xf1206                              # software breakpoint at linear 0xf1206
(gdb) c                                       # continue
Breakpoint 1, 0x000f1206 in ?? ()             # ← stub fired the bp

(gdb) set *(unsigned char*)0xf1206 = 0x90     # patch a byte (NOP it)

(gdb) c                                       # let it run
^C                                            # interrupt: gdb sends 0x03; stub stops
Program received signal SIGINT.

(gdb) detach                                  # release; DOSBox keeps running
```

## Address convention — read this once

GDB has no notion of x86 segments. We hide them: the stub presents
**linear addresses** (the post-segmentation, post-paging address that
the CPU's load/store unit ultimately uses) for both `eip` and
breakpoint addresses.

For real mode that is `(CS << 4) + IP`.
For protected mode it is the descriptor base plus offset (resolved by
DOSBox's existing `GetAddress()` helper).

So if your COM file is loaded with `CS = 0x10b3` and the entry point is
at offset `0x100`, the address you give gdb is **`0x10c30`**, not
`0x100` and not `0x10b30100`. To find the right linear value before
you've broken in, the easy way is:

1. Attach.
2. `info reg cs eip` — gdb shows you both as 32-bit values; `eip` is
   already linearised.
3. Use that as the basis for `b *<addr>`.

Writing to `eip` (e.g. `set $eip = 0xabcd`) is also linear: the stub
splits the value back into `(linear - SegPhys(cs))` for `reg_eip`.

## Limitations and gotchas

- **Single-thread only.** The stub reports `thread 1` for everything.
  `info threads` will show one thread.
- **No hardware watchpoints.** `Z2`/`Z3`/`Z4` packets reply empty.
  Software breakpoints (`Z0`/`Z1`) work.
- **No `vCont`.** gdb falls back to plain `c`/`s`. Functional, but
  modern gdb may emit warnings about missing capabilities.
- **No target XML.** gdb uses its built-in i386 register layout. Don't
  try `set arch x86_64` — registers aren't packed for 64-bit.
- **dynrec slowdown while breakpoints are armed.** When you set the
  first breakpoint, the stub forces the dynrec JIT to fall back to the
  interpreter so per-instruction breakpoint checks fire reliably. When
  you clear the last breakpoint and aren't single-stepping, dynrec is
  restored. Expect a visible perf cliff in JIT-heavy guests during a
  debug session.
- **gdb still wants you to set the architecture.** Without `set arch
  i386` (or `i8086`), gdb may default to your host triplet and produce
  garbage disassembly.
- **The curses debugger is still present.** A `--enable-debug=heavy`
  build always includes the Alt-Pause curses TUI alongside the gdb
  stub. They don't conflict in normal use, but if you press Alt-Pause
  while gdb is attached you'll have two parties trying to drive the
  CPU; just use one at a time.

## Troubleshooting

**`./configure: error: --enable-gdbserver requires SDL_net`**
You installed SDL2_net (`libsdl2-net-dev`). The DOSBox 0.74 line is
SDL 1.2; you need `libsdl-net1.2-dev`.

**gdb hangs at `Remote replied unexpectedly to 'vMustReplyEmpty':
timeout`**
Almost always: there's a stale DOSBox process still bound to the port.
`pgrep -af dosbox` and kill it. The stub's listener accepts only one
client at a time; a stranded one will wedge the next attempt.

**Breakpoint set but never fires.**
The address is probably wrong. `info reg eip` after attach shows you
the linearised PC; cross-check that your `b *0x…` matches the same
address space. Real-mode `(CS<<4)+IP`, not raw IP.

**`si` always shows the same `$eip`.**
Likely you're attached but the guest is in a tight loop containing the
same instruction (e.g. a `JMP $` halt). Try `b` somewhere else and
`c`, or `set $eip = …` to escape.

**Memory read returns `E14`.**
The address is unmapped or paging refuses access. The stub uses
`mem_readb_checked` — same view of memory as the guest CPU has. If the
guest can't read it, neither can you.

**`Ctrl-C` in gdb does nothing.**
Polling cadence is once every ~1024 guest instructions, so on a really
idle guest it can take a beat. If it never works, check that
`gdb_client_sock` is non-null (the trace logging in `gdbserver.cpp`
that we removed was useful for this — re-add it locally if needed).

## Implementation pointers

If you need to extend the stub:

- `include/gdbserver.h` — public API.
- `src/gdbserver/gdb_internal.h` — shared state across the three .cpp files.
- `src/gdbserver/gdbserver.cpp` — lifecycle (`GDB_Init`/`Shutdown`), the
  per-instruction hook (`GDB_HeavyCheck`), `GDB_Loop`, and packet
  dispatch.
- `src/gdbserver/gdb_packet.cpp` — RSP framing, hex helpers.
- `src/gdbserver/gdb_target.cpp` — register pack/unpack, memory accessors,
  software-breakpoint table.
- The per-instruction hook is wired into all six CPU cores. Search for
  `GDB_HeavyCheck` to find every site.

A wider architectural orientation lives in `AGENT.md`.
