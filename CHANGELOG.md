# Changelog

All notable changes to this DOSBox fork are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Upstream
DOSBox 0.74-3 is the baseline; only deltas against that baseline are
listed.

## [Unreleased]

### Added

- **GDB remote-serial-protocol stub.** New module `src/gdbserver/`
  exposes the running DOS guest to an external `gdb` over TCP. Enabled
  via the new `--enable-gdbserver` configure flag (which itself
  requires `--enable-debug=heavy` and SDL_net 1.2). Activate at run
  time through the new `[gdbserver]` section in `dosbox.conf`
  (`enabled`, `port`, `wait_for_connection`) or the new command-line
  flags `-gdbserver [port]` and `-gdbserver-wait`. Supports the core
  RSP packet set (`?`, `g`, `G`, `p`, `P`, `m`, `M`, `X`, `c`, `s`,
  `Z0/z0`, `qSupported`, `qC`, `H`, `qfThreadInfo`, `qsThreadInfo`,
  `qAttached`, `qOffsets`, `D`, `k`); software breakpoints, single-
  step, async Ctrl-C interrupt, and read/write of registers and
  memory all work. Addresses are exposed to gdb as linear (post-
  segmentation) values for both real and protected modes. While any
  breakpoint is armed (or single-step is pending) the dynrec JIT is
  forced down to the interpretive core for correctness. See
  `docs/gdb-debugging.md` for full usage and `AGENT.md` for the
  architectural pointer.

- **`AGENT.md`** at the repository root — orientation for AI agents
  joining the project. Includes the requirement that all user-visible
  changes be recorded here in `CHANGELOG.md`.

- **`docs/gdb-debugging.md`** — user-facing documentation for the GDB
  stub: prerequisites, build, activation, RSP background, typical
  session, address convention, limitations, troubleshooting.

### Changed

- **`README`** — documents the new `-gdbserver` and `-gdbserver-wait`
  command-line flags.
- **`INSTALL`** — documents the new `--enable-gdbserver` configure
  flag and clarifies that SDL_net 1.2 (not SDL2_net) is required.

### Fixed

- **`src/debug/debug_gui.cpp`** — replaced direct accesses to the
  `WINDOW->_begy` field with calls to the `getbegy()` accessor. Modern
  ncurses makes the `WINDOW` struct opaque, so the original code
  failed to build against any current Linux distribution's ncurses.
  This fix was required to build `--enable-debug=heavy` at all (and
  therefore to build `--enable-gdbserver`).

[Unreleased]: about:blank
