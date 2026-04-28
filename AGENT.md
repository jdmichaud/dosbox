# AGENT.md — orientation for AI agents working on this DOSBox fork

This file is a **living document**. Update it whenever your understanding of
the project changes, or when you add/move/rename a major piece. Future you
(and other agents) will rely on it as a starting point — keep it accurate.

## What this is

This repository is a fork of **DOSBox 0.74-3**, the upstream DOS emulator
(see `README` for the user-facing description). It emulates an x86 PC
(roughly Pentium-class) running DOS, plus the surrounding hardware
(VGA/SVGA, Sound Blaster, Adlib, CD-ROM, IPX, etc.) so that DOS programs
and games can run on modern hosts. Upstream is GPL-2.0-or-later.

We track our own modifications in `CHANGELOG.md` (see *Workflow* below).

## High-level architecture

DOSBox is single-threaded, event-driven, and built around a swappable
"main loop" indirection. The interesting pieces:

- **Main loop indirection** (`src/dosbox.cpp`). `DOSBOX_RunMachine()`
  repeatedly invokes the function pointer `loop`. The default is
  `Normal_Loop()`, which (a) runs the current CPU core, (b) handles
  callback returns, (c) pumps SDL events. Other loops (e.g. the curses
  debugger's `DEBUG_Loop`, or our `GDB_Loop`) get installed via
  `DOSBOX_SetLoop()` and take over the main loop until they hand control
  back via `DOSBOX_SetNormalLoop()`.

- **CPU cores** (`src/cpu/`). Six interchangeable interpreters/JITs share
  the `cpudecoder` function pointer. From slowest/most-portable to
  fastest:
  - `core_full.cpp`      — full table-driven interpreter
  - `core_normal.cpp`    — generated normal interpreter
  - `core_simple.cpp`    — like normal, simpler
  - `core_prefetch.cpp`  — interpreter with prefetch queue model
  - `core_dyn_x86.cpp`   — x86-only dynamic recompiler (legacy)
  - `core_dynrec.cpp`    — portable dynamic recompiler (default on most builds)
  Each core has the same per-instruction hook site (look for
  `DEBUG_HeavyIsBreakpoint` to find them) — this is where the curses
  debugger and the gdb stub get a chance to halt before each guest
  instruction. The dynrec cores call the hook between blocks, not
  between instructions, so single-step needs to force the interpreter
  (see `src/gdbserver/gdbserver.cpp::gdb_apply_core_policy`).

- **Memory** (`include/mem.h`, `include/paging.h`, `src/cpu/paging.cpp`,
  `src/hardware/memory.cpp`). Always-`PhysPt` (32-bit linear) addresses
  go through `mem_readb/w/d` (and `_checked` variants for fault-aware
  use). Real-mode `(seg<<4)+off`, protected-mode descriptor lookup, and
  software paging are all hidden behind these accessors.

- **Registers** (`include/regs.h`). Direct globals: `reg_eax`, `reg_eip`,
  `reg_flags`, `Segs.val[cs/ds/...]`, `Segs.phys[...]`. `Bitu` is
  host-word-size; cast to `Bit32u` when serialising.

- **DOS / BIOS layer** (`src/dos/`, `src/ints/`, `src/shell/`). The
  in-process implementation of DOS, BIOS, INT 21h, command line, etc.
  This is what the guest sees as the operating system.

- **Hardware** (`src/hardware/`). Audio, video, joystick, serial, IPX,
  CD-ROM, etc. Each device is a separate module with an `Init()` /
  optional destroy function registered via the section system.

- **Section / config system** (`src/misc/setup.cpp`, headers in
  `include/setup.h` and `include/control.h`). DOSBox's `dosbox.conf` is
  parsed into named `Section_prop` instances; each section has typed
  properties (`Add_bool`, `Add_int`, `Add_string`, `Add_hex`) and an
  init function that runs once, in registration order, during
  `control->Init()`. Command-line overrides are applied by mutating the
  relevant section via `HandleInputline()` *before* its init runs — see
  `DOSBOX_RealInit` in `src/dosbox.cpp` for the pattern.

- **Callback mechanism** (`include/callback.h`, `src/cpu/callback.cpp`).
  Lets host code surface as guest INT handlers / device entry points.
  Cores can `return <callback_id>` to dispatch into a host function from
  the main loop.

- **GUI / SDL** (`src/gui/sdlmain.cpp`). SDL 1.2-based window, input,
  scaling. `GFX_Events()` pumps SDL; loops that hold the CPU offline
  (debugger, gdb stub) must call it periodically so the window stays
  responsive.

- **GDB stub** (`src/gdbserver/`, public header `include/gdbserver.h`).
  Our addition. Implements the GDB Remote Serial Protocol over TCP so an
  external `gdb` can debug the DOS guest. Hooked into every CPU core
  alongside the heavy-debug check. See `docs/gdb-debugging.md` for user
  documentation. Compiled in only when `--enable-gdbserver` is passed
  (which itself requires `--enable-debug=heavy` and SDL_net 1.2).

## Build

DOSBox uses GNU autotools. The release tarball ships generated
`configure`, `Makefile.in`, `config.h.in`, etc.; `./autogen.sh`
regenerates them from the `*.am` / `.ac` sources.

```sh
./autogen.sh                  # only when changing configure.ac or Makefile.am
./configure [options]
make -j$(nproc)
src/dosbox                    # built binary
```

Useful `./configure` options (see `INSTALL` for the full list):

| Flag                     | Effect                                                                 |
|--------------------------|------------------------------------------------------------------------|
| `--enable-debug`         | Enable curses-based built-in debugger (Alt-Pause). Requires libcurses. |
| `--enable-debug=heavy`   | Plus per-instruction hooks, instruction logging, etc.                  |
| `--enable-gdbserver`     | Build the GDB remote stub. Requires `=heavy` and SDL_net 1.2.          |
| `--disable-dynamic-x86`  | Don't build the x86-specific JIT (default on x86_64).                  |
| `--disable-dynrec`       | Don't build the portable JIT.                                          |
| `--disable-fpu`          | Drop x87 emulation (don't, unless you know why).                       |

Build artifacts (`config.h`, `Makefile`, `*.o`, `*.a`, `src/dosbox`)
are in `.gitignore` — do not commit them. `Makefile.in` and
`config.h.in` *are* committed (shipped with the upstream tarball).

## Where to put things

- New emulated hardware → `src/hardware/<name>.cpp` plus an Init function
  registered in `src/dosbox.cpp::DOSBOX_Init`.
- New DOS service or BIOS interrupt → `src/dos/` or `src/ints/`.
- New CPU feature → touch every core that needs it; they share patterns
  but not code.
- New conf section → `Section_prop *secprop = control->AddSection_prop(...)`
  in `DOSBOX_Init`, with the init function declared near the top of
  `src/dosbox.cpp`.
- New cmdline flag → parse with `control->cmdline->FindInt/FindExist/...`
  in `DOSBOX_RealInit` (early, before other sections init), and translate
  into a `HandleInputline()` call on the relevant section so config-file
  and cmdline behaviour stay consistent.

## Coding conventions

DOSBox's style is older C++ (pre-C++11), tabs for indent, K&R braces,
mixed naming (PascalCase for types, snake_case for some functions,
PascalCase for others). It is not consistent. Match the surrounding
file rather than imposing a global style. Prefer the existing helpers
(`mem_readb_checked`, `LOG_MSG`, etc.) over rolling your own.

`LOG_MSG(fmt, ...)` is the standard logging macro. Note: when built with
`--enable-debug`, it expands to `DEBUG_ShowMsg` which writes to the
curses output pane, *not* to stderr — so during heavy-debug builds the
log is invisible unless you're inside the curses TUI. For temporary
diagnostics outside the TUI, write to a file directly (we removed all
such temporary scaffolding from the gdb stub before committing).

## Workflow

- **Track every user-visible change in `CHANGELOG.md`.** New flags, new
  conf sections, new features, behaviour changes, fixed bugs that users
  would notice. Use Keep-a-Changelog format. We don't yet do versioned
  releases on this fork — append to `[Unreleased]`.
- **Update this file (`AGENT.md`) when your understanding grows.** If
  you discover an architectural detail that wasn't here, that surprised
  you, or that future agents would benefit from, add it. If you move,
  rename, or delete a module mentioned here, update the reference.
- **Update `docs/`** when the user-facing behaviour of a feature
  changes. The gdb stub has its own page (`docs/gdb-debugging.md`); add
  similar pages for other non-trivial features rather than dumping
  everything in `README`.
- **Don't introduce style churn.** This codebase is 20+ years old; mass
  reformatting buries real changes in noise.

## Pointers

- Upstream DOSBox: https://www.dosbox.com (the canonical source for
  unmodified 0.74-3 behaviour).
- GDB Remote Serial Protocol reference:
  https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html
- Our gdb stub usage: `docs/gdb-debugging.md`.
- Build/install instructions for end users: `INSTALL`.
- Cmdline reference: `README`.
