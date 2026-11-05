# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

LazyDaysOS (a.k.a. DudeOS) is a hobby 32-bit x86 kernel in C and GNU `as` (AT&T syntax), booted by GRUB via **Multiboot v1**. It sets up a GDT, remaps the 8259 PICs, installs an IDT, builds a physical frame allocator from the Multiboot memory map, enables paging, creates a kernel heap (`kmalloc`/`kfree`), runs a 100 Hz PIT timer, runs boot-time self-tests, and echoes keyboard input to the VGA text console. Kernel messages are mirrored to COM1. There is no userspace or scheduler yet.

## Toolchain

- Requires an **`i686-elf` cross compiler**. The host `gcc` will not work: `libk/include/tty.h` `#error`s if `__linux__` is defined or `__i386__` is not.
- On this machine: binutils 2.47 + GCC 15.3.0 (C only, with libgcc) installed in `~/opt/cross`, with `~/opt/cross/bin` added to `PATH` in `~/.bashrc`. Sources and the rebuild script live in `~/opt/cross-build/build.sh`.
- In a non-interactive shell, prefix commands with `export PATH="$HOME/opt/cross/bin:$PATH";` if `i686-elf-gcc` is not found.
- Other tools: `grub-mkrescue` + `grub-pc-bin` (BIOS `i386-pc` modules are required, since EFI-only GRUB makes an unbootable ISO for this kernel), `xorriso`, `mtools`, `grub-file`, `qemu-system-i386`, `gdb`. No `nasm`; the code uses GNU `as`. The README holds the cross-compiler build recipe.
- `.vscode/c_cpp_properties.json` resolves the toolchain via `${env:HOME}/opt/cross` and hard-codes the GCC version (`15.3.0`) in the include path. Update it if the toolchain is rebuilt with a different version.

## Build / run / debug

```bash
make               # -> ./LazyDaysOS.bin (objects/archives land in the repo root)
make check         # boots ./LazyDaysOS.bin headless in QEMU test mode; exit 0 = all self tests passed
make install       # copies libk + headers + kernel into sysroot/, builds ISO, runs multiboot check
./bootit.sh        # qemu -kernel sysroot/boot/kernel/LazyDaysOS.bin, serial on this terminal
./bootit.sh --iso  # qemu -cdrom sysroot/boot/kernel/LazyDaysOS.iso (GRUB boots immediately)
./bootit.sh -d     # adds qemu -s -S (waits for gdb on :1234); combinable with --iso
./debug.sh         # opens two gnome-terminal windows: qemu -d + `gdb --command=gdbconf` (breaks at _start)
make clean
```

- **Run `make check` after every kernel change.** It passes `test` on the kernel command line, and the kernel then reports its self-test result through QEMU's `isa-debug-exit` device (port `0xF4`): QEMU exit status 1 = pass, 3 = a test failed or the kernel panicked. `check_qemu.sh` prints the serial log (also saved to `qemu_check.log`) and also catches hangs (30 s timeout) and triple faults (`-no-reboot`).
- `bootit.sh` and `debug.sh` read from **`sysroot/`**, so run `make install` (not just `make`) before booting.
- The `.iso` rule deletes `./LazyDaysOS.bin` and `./LazyDaysOS.iso` after copying them into `sysroot/boot/kernel/`. The next `make` relinks.
- Header dependencies are tracked with `-MMD -MP` (`*.d` files next to the objects).
- `make test` checks the image with `grub-file --is-x86-multiboot` and fails the build if it's invalid.
- The build is warning-free; keep it that way.

### Headless verification (for agents)

Prefer `make check`. To look at the screen or type into a normal boot without a display:

```bash
qemu-system-i386 -kernel sysroot/boot/kernel/LazyDaysOS.bin -display none -no-reboot \
  -monitor tcp:127.0.0.1:45454,server,nowait
```

Then over the monitor socket: `sendkey <key>` to type (and `sendkey ret` to get past the GRUB menu for `-cdrom`), and `xp /2000hx 0xb8000` to dump the screen. Use the low byte of each cell as ASCII. Avoid long `-monitor unix:` paths (108-byte limit). `-d int -D log` traces interrupts.

Add `-serial stdio` (or `-serial file:x.log`) to read the console text directly. A healthy boot prints `[K_INFO] Initializing IDT` … `[K_SUCC] self tests passed ... Done` → `[K_SUCC] getting kernel ready... Done`, then echoes typed lowercase letters, digits, space, and Enter (VGA only; keyboard echo is not mirrored to serial).

## Build layout (Makefile)

- `CC=i686-elf-gcc -m32`, `CFLAGS=-nostdlib -ffreestanding -D __LF -Ilibk/include -Ikernel/compat/i386/include -g -O0`. Link: `-T kernel/compat/i386/linker.ld ... -lgcc`.
- Three object groups: `KERN_OBJ` (kernel + compat), `LIBK_OBJS` → `libk.a` (tty, kio, ttyin, serial), `UTIL_OBJS` → `util.a` (memory).
- **Adding a source file:** append its `.o` to the right group and its directory to `VPATH`. Compat files are referenced with a path prefix (`$(COMPAT_DIR)/foo.o`), so their objects stay next to the source.
- `linker.ld`: load at 1 MiB; `.text` (with `.multiboot` first), `.rodata`, `.data`, `.bss`, all 4K-aligned, wildcards for `.rodata.*` etc. Entry `_start`. `_kernel_start`/`_kernel_end` bound the image; the frame allocator reserves that range.

## Code map and boot flow

```
kernel/boot.s              Multiboot header, 16 KiB stack in .bss, _start
kernel/hwio.s              outb/inb, idt_buf_drain (lidt), i686_GDT_Load
kernel/gdt.c/.h            GDT: null, ring0 code 0x08, ring0 data 0x10, two ring3 entries
kernel/idt.c/.h            PIC remap (IRQ0-15 -> vectors 0x20-0x2F), IDT gates 0-47, init_idt()
kernel/idt_hndlr_setup.s   ISR0-31 / IRQ0-15 stubs -> general_interrupt_handler -> interrupt_handler(struct int_frame*)
kernel/isr.c/.h            EOI, dispatch through isr_map[], panic on unhandled CPU exceptions, isr_self_test
kernel/panic.c/.h          k_panic / k_panic_frame: red register dump (+CR2 on #PF), then halt
kernel/heap.c/.h           kmalloc/kfree: first-fit heap over 4 MiB of contiguous frames
kernel/kernel.c            kernel_main(magic, mbi)
kernel/compat/i386/        arch-specific:
  kern_powerup.c             powerup_kernel_i386 (bring-up order), kern$self_test, test mode + QEMU exit
  pmm.c                      physical frame bitmap allocator from the Multiboot memory map
  vm_watchdog.c              paging setup
  pit.c                      PIT channel 0 at 100 Hz, tick counter, pit_sleep_ms
  include/multiboot.h        Multiboot v1 info / mmap structures
libk/tty/tty.c             VGA text console 80x25 at 0xB8000 (terminal_*)
libk/tty/serial.c          polled COM1 output, 115200 8N1
libk/kio/kio.c             k_print -> VGA + COM1; supports %% %c %u %d %x %s (no width/padding)
libk/tty/ttyin.c           scancode set 1 -> ASCII, keyboard_read() (gated by input_allowed)
libk/util/memory.c         lazy$block helpers + util$self_test()
```

Boot sequence:

1. GRUB enters `_start` in 32-bit protected mode with EAX = Multiboot magic, EBX = info pointer. `_start` sets up its own stack, pushes both, calls `i686_GDT_Initialize()` (loads the GDT, reloads segments, far-returns to reload CS) and then `kernel_main(magic, mbi)`.
2. `kernel_main`: `terminal_initialize()`, `serial_initialize()`, check the magic, `init_idt()` (PIC remap, gates, `lidt`, `register_routines()`).
3. `powerup_kernel_i386(mbi)`, still with interrupts off: parse the command line for `test`, `mem$pmm_init` (must run **before** paging; reads Multiboot data physically), `mem$setup_paging`, `mem$heap_init`, `pit_initialize`.
4. `sti`, then `kern$self_test()` (util, PMM, heap, ISR register preservation, timer). In test mode, exit QEMU with the result.
5. Set `input_allowed = true` and idle in a `hlt` loop. Everything afterwards happens in IRQ handlers (timer IRQ0, keyboard IRQ1).

Memory layout (identity mapped, virtual = physical): page 0 is **not present** (NULL dereferences panic with `#PF`, `cr2=0x0`); 4 KiB–4 MiB uses 4 KiB pages; 4 MiB–4 GiB uses 4 MiB PSE pages, uncached above the end of RAM. Frames below 1 MiB, the kernel image and the Multiboot structures are reserved. With QEMU's default 128 MiB, the heap lands right after the kernel image.

### Things to know before editing low-level code

- Unhandled CPU exceptions (vectors 0–31 with no `isr_map` entry) panic; returning would re-run the faulting instruction. IRQ handlers in `isr_map` are `void (*)(void)` and run **after** the EOI is sent.
- Stubs push a dummy error code for exceptions without one, so `struct int_frame` is uniform. Error-code vectors: 8, 10–14, 17, 21, 29, 30.
- `pit_sleep_ms` needs interrupts enabled (it panics otherwise). Self tests that need the timer must run after `sti`.
- `k_print` writes to serial only after `serial_initialize()`; anything printed earlier is VGA-only.
- `vgadef.h` defines `static` terminal state (`terminal_row`, `terminal_buffer`, …) in a header, so every TU gets its own copy. Only touch them from `tty.c`.
- The README claims an MIT license, but there is no `LICENSE` file.

## Conventions

- **`facility$name` naming** for functions, types, struct fields and status codes (`mem$setup_paging`, `util$blkcpy_weak`, `struct lazy$block`, `lb$base_addr`, `UTIL$SUCCESS`, `KERN$PWRUP`, `LAZY$BLOCKINV`). `facility$$name` marks internal/static helpers and tests (`util$$blkcpy_weak_test`). GCC accepts `$` in identifiers; keep using it.
- Status codes: success is odd (`UTIL$SUCCESS 0x1`, `UTIL$FAIL 0x0`). Return `size_t`/`uint16_t` statuses rather than `bool`.
- Kernel log lines are prefixed `[K_INFO]`, `[K_SUCC]`, `[K_FAIL]` (`[UTIL_FAIL]` inside util) and printed with `k_print`.
- Header guards are `#ifndef __NAME__` style (some files also use `#pragma once`). Doxygen-style `@brief` comments appear on non-trivial functions.
- Indentation is mixed (tabs in `libk/tty`, `kio`, `kernel.c`; 4 spaces in `memory.c`, `vm_watchdog.c`; 2 spaces in `idt.c` gate setup). Match the file you're editing.
- Arch-specific code goes under `kernel/compat/i386/` with headers in its `include/`. Generic kernel headers sit next to their `.c` and are included with quotes.
- There is no host-side test suite. Tests are boot-time self-tests run by `kern$self_test()` (`kernel/compat/i386/kern_powerup.c`) and checked by `make check`. Each module exports a `*_self_test()` returning `UTIL$SUCCESS`/`UTIL$FAIL` and prints a `[UTIL_FAIL]` line explaining any failure; add new ones to `kern$self_test()`. When fixing a bug, add a test that fails on the old code.
