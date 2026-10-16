# CPUs without MOVBE, BMI1 or LZCNT (2026-10-15)

The image the port runs (eboot.bin linked with its modules) uses Haswell instructions: MOVBE,
BMI1 (ANDN/BEXTR/BLS*) and LZCNT. Sandy Bridge and Ivy Bridge CPUs (issue #31: i5-3570, i7-3770)
have the rest of the port's baseline (SSE4.2, POPCNT and AVX with Ivy Bridge) but not these sets,
and the game stopped at the first one with an illegal instruction (`STOP` in the log). Emulating
each execution costs far too much, so the port rewrites the instructions in the image once, at
load: the design of gow3_pc's G3PROBE5 rewrite (`FelipePrado0/gow3_pc`, GPL-2.0; the table format
is renamed BBISA1).

## Table

`scripts/isa_rewrite.py` (iced-x86) decodes each executable segment of `out/boot-linked.bin` and
classifies the sites:

- **JMP**: 5 bytes or more; the loader writes a 5-byte `jmp rel32` to the site's stub.
- **JMP_RELOCATE**: 4 bytes with a plain following instruction (not a branch target): the stub
  runs the site and a copy of that instruction.
- **INT3**: 4 bytes otherwise; the loader writes `int3` and the fault path emulates it.
- **UNSUPPORTED**: encodings the stubs do not cover (segment override, LOCK, 32-bit addressing,
  F16C). Reported always, fatal only when the host needs that set.

`out/isa-rewrite.bin` (BBISA1) starts with the image size and SHA-256, then one 64-byte record per
site (kind, operand registers, memory operand, flags, mode, original and relocated bytes).
`run.py` generates it only when `sacredfire.exe --cpu-features` says the host lacks MOVBE, BMI1,
LZCNT or F16C (a quick second run of the executable at launch).

## Load

The probe verifies the table's SHA-256 against the image exactly as read from `boot-linked.bin`
and, after the port's own patch work (patch sites, TCB loads, GPU patch), rewrites each site whose
bytes still match, degrading to `int3` (emulated) or to a counted skip when they do not. The
stubs are allocated together with the image and left readable and executable. The log carries one
line:

```
ISA: CPU lacks MOVBE, BMI1, LZCNT: 4243 sites rewritten (4004 jmp, 231 relocated, 8 int3), 0 skipped
```

## Stubs and traps

`src/isa_emulate.c` runs one instruction per stub call with the guest's registers (SysV ABI;
compiled with `-mgeneral-regs-only` because it executes inside guest code and must not touch
XMM/YMM). The same code is the worker of the int3 trap: the fault handler recognizes a site or
stub address, emulates, restores flags and resumes after the site. In CTR 1.21 only 8 sites need
the trap.

## Diagnostics

| Variable | Effect |
|---|---|
| `BB_ISA_REWRITE=1` | generate and apply the table on any CPU (testing) |
| `BB_ISA_REWRITE=0` | never rewrite: the first MOVBE/BMI1/LZCNT stops the process again |
| `BB_SKIP_CPU_CHECK=1` | skip the fatal-set check (SSE4.2, POPCNT, AVX), as before |

`--cpu-features` prints `CPU: missing ...` or `CPU: complete` and exits 20 when a set no table
covers is missing.

## Packaging

The Python that runs `run.py` needs iced-x86 (`pip install iced-x86`; its wheels are cp38-abi3 and
a wheel is a zip): `packaging/windows/make_portable.py` bundles it in the portable ZIP. The Linux
AppImage does not carry it yet (iced-x86 is not in nixpkgs): there the rewrite logs why it is
unavailable, which only affects CPUs that need it.

## Tested

i7-3770 (Ivy Bridge) with an RX 580, Windows: 4243 sites (MOVBE 391, BMI1 3528, LZCNT 324), 231
relocated, 8 int3, 0 unsupported, 0 skipped; the game boots to the start screen and runs.
