# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

LazyDaysOS (a.k.a. DudeOS) is a hobby 32-bit x86 kernel in C and GNU `as` (AT&T syntax), booted by GRUB via **Multiboot v1**. It sets up a GDT, remaps the 8259 PICs, installs an IDT, identity-maps the first 4 MiB with paging, runs in-kernel self-tests, and echoes keyboard input to the VGA text console. There is no userspace, scheduler, or allocator yet.

## Toolchain

- Requires an **`i686-elf` cross compiler**. The host `gcc` will not work: `libk/include/tty.h` `#error`s if `__linux__` is defined or `__i386__` is not.
- On this machine: binutils 2.47 + GCC 15.3.0 (C only, with libgcc) installed in `~/opt/cross`, with `~/opt/cross/bin` added to `PATH` in `~/.bashrc`. Sources and the rebuild script live in `~/opt/cross-build/build.sh`.
- In a non-interactive shell, prefix commands with `export PATH="$HOME/opt/cross/bin:$PATH";` if `i686-elf-gcc` is not found.
- Other tools: `grub-mkrescue` + `grub-pc-bin` (BIOS `i386-pc` modules are required, since EFI-only GRUB makes an unbootable ISO for this kernel), `xorriso`, `mtools`, `grub-file`, `qemu-system-i386`, `gdb`. No `nasm`; the code uses GNU `as`. The README holds the cross-compiler build recipe.
- `.vscode/c_cpp_properties.json` resolves the toolchain via `${env:HOME}/opt/cross` and hard-codes the GCC version (`15.3.0`) in the include path. Update it if the toolchain is rebuilt with a different version.

## Build / run / debug

```bash
make               # -> ./LazyDaysOS.bin (objects/archives land in the repo root)
make install       # copies libk + headers + kernel into sysroot/, builds ISO, runs multiboot check
./bootit.sh        # qemu -kernel sysroot/boot/kernel/LazyDaysOS.bin
./bootit.sh --iso  # qemu -cdrom sysroot/boot/kernel/LazyDaysOS.iso (press Enter at GRUB menu: no timeout set)
./bootit.sh -d     # same as default but qemu -s -S (waits for gdb on :1234)
./debug.sh         # opens two gnome-terminal windows: qemu -d + `gdb --command=gdbconf` (breaks at _start)
make clean
```

- `bootit.sh` and `debug.sh` read from **`sysroot/`**, so run `make install` (not just `make`) before booting.
- `bootit.sh` runs `sudo qemu-system-i386`; sudo is not needed. Call `qemu-system-i386` directly instead (sudo prompts for a password in this environment).
- `bootit.sh` only inspects `$1`, so `-d` and `--iso` can't be combined.
- The `.iso` rule deletes `./LazyDaysOS.bin` and `./LazyDaysOS.iso` after copying them into `sysroot/boot/kernel/`. The next `make` relinks.
- Rerunning `make install` packs the previous `sysroot/boot/kernel/LazyDaysOS.iso` into the new ISO, which grows over time. Remove `sysroot/` (or `make clean`) first if that matters.
- **No header dependency tracking.** After editing a `.h`, run `make clean`.
- `make clean` only removes root-level `*.o`; it misses `kernel/compat/i386/*.o`. Delete those manually if they're stale.
- `make test` checks the image with `grub-file --is-x86-multiboot`. The script always exits 0, so read its output.
- The only expected compiler warning: `'input_allowed' initialized and declared 'extern'` in `kernel/kernel.c`.

### Headless verification (for agents)

There is no serial output; everything goes to VGA text memory at `0xB8000`. To check a boot without a display:

```bash
qemu-system-i386 -kernel sysroot/boot/kernel/LazyDaysOS.bin -display none -no-reboot \
  -monitor tcp:127.0.0.1:45454,server,nowait
```

Then over the monitor socket: `sendkey <key>` to type (and `sendkey ret` to get past the GRUB menu for `-cdrom`), and `xp /2000hx 0xb8000` to dump the screen. Use the low byte of each cell as ASCII. Avoid long `-monitor unix:` paths (108-byte limit). `-d int -D log` traces interrupts.

A healthy boot prints `[K_INFO] Initializing IDT` … `[K_SUCC] utility self test passed ... Done` → `[K_SUCC] getting kernel ready... Done`, then echoes typed lowercase letters, digits, space, and Enter.

## Build layout (Makefile)

- `CC=i686-elf-gcc -m32`, `CFLAGS=-nostdlib -ffreestanding -D __LF -Ilibk/include -Ikernel/compat/i386/include -g -O0`. Link: `-T kernel/compat/i386/linker.ld ... -lgcc`.
- Three object groups: `KERN_OBJ` (kernel + compat), `LIBK_OBJS` → `libk.a` (tty, kio, ttyin), `UTIL_OBJS` → `util.a` (memory).
- **Adding a source file:** append its `.o` to the right group and its directory to `VPATH`. Compat files are referenced with a path prefix (`$(COMPAT_DIR)/foo.o`), so their objects stay next to the source.
- `linker.ld`: load at 1 MiB; `.text` (with `.multiboot` first), `.rodata`, `.data`, `.bss`, all 4K-aligned. Entry `_start`.

## Code map and boot flow

```
kernel/boot.s              Multiboot header, 8000-byte stack in .bss, _start
kernel/hwio.s              outb/inb, idt_buf_drain (lidt), i686_GDT_Load
kernel/gdt.c/.h            GDT: null, ring0 code 0x08, ring0 data 0x10, two ring3 entries
kernel/idt.c/.h            PIC remap (IRQ0-15 -> vectors 0x20-0x2F), IDT gates 0-47, init_idt()
kernel/idt_hndlr_setup.s   ISR0-31 / IRQ0-15 stubs -> general_interrupt_handler -> interrupt_handler(intno)
kernel/isr.c/.h            EOI + dispatch through isr_map[] (register_routines())
kernel/kernel.c            kernel_main
kernel/compat/i386/        arch-specific: kern_powerup.c (powerup_kernel_i386), vm_watchdog.c (paging), linker.ld
libk/tty/tty.c             VGA text console 80x24 at 0xB8000 (terminal_*)
libk/kio/kio.c             k_print: supports %% %c %u %d %s only (no %x)
libk/tty/ttyin.c           scancode set 1 -> ASCII, keyboard_read() (gated by input_allowed)
libk/util/memory.c         lazy$block helpers + util$self_test()
```

The actual boot sequence (confirmed in GDB) differs from what `_start` looks like:

1. GRUB enters `_start` already in 32-bit protected mode. `_start` does `cli` and calls `i686_GDT_Initialize()` → `i686_GDT_Load` (`hwio.s`).
2. `i686_GDT_Load` pops its return address and argument, does `lgdt`, reloads DS/ES/FS/GS/SS with 0x10, then **`retf`s to `0x08:kernel_main`**. It never returns, so the rest of `_start` (`safe_switch_protected`, `sti`, `call kernel_main`, `hlt` loop) is dead code. `kernel_main`'s return slot holds `0x00000008`.
3. `kernel_main`: `terminal_initialize()` → `init_idt()` (PIC remap, gates, `lidt`, `register_routines()`) → `powerup_kernel_i386()` (reads CR0, `mem$setup_paging()` identity-maps 0–4 MiB and sets CR0.PG, runs `util$self_test()`, sets `input_allowed = true`).
4. `kernel_main` ends with `int $55`. Vector 55 has no IDT gate, so this raises #GP (vector 13). The no-op handler `iret`s back to the faulting instruction, so **the kernel's idle state is an endless `int $55` → #GP loop**, with timer and keyboard IRQs serviced in between. Don't mistake this for a new bug, but anything placed after `int $55` never runs.

### Known quirks worth knowing before editing low-level code

- `idt_buf_drain` does `lidt (%eax)` and ignores its stack argument. It works only because the caller happens to leave the address in EAX at `-O0`. Changing optimisation or the call site can break it.
- `safe_switch_protected` (`or %eax, 0x1`) is a memory OR to address 1 in AT&T syntax, not a set-bit. It's harmless only because it's unreachable.
- `PGFLT_INTERRUPT` is `MAP_IRQ(14)` = vector 46 (IDE IRQ14), **not** CPU exception 14. `pagefault_hander` is never invoked by real page faults.
- Error-code exceptions: only 8 and 10–14 are wired as `with_errcode`. 17, 21, 29 and 30 also push an error code on real hardware but use the no-errcode stub, so the stack would be misaligned if they fire.
- `interrupt_handler` doesn't use the saved register state; handlers are `void (*)(void)`.
- `vgadef.h` defines `static` terminal state (`terminal_row`, `terminal_buffer`, …) in a header, so every TU gets its own copy. Only touch them from `tty.c`.
- `VGA_HEIGHT` is 24, not 25; the bottom row is unused. Scrolling only triggers on line wrap, not on `\n`, so output past row 24 is not scrolled.
- The stack is 8000 bytes, despite the "16KiB" comment in `boot.s`.
- The README claims an MIT license, but there is no `LICENSE` file.

## Conventions

- **VMS-style naming**: `facility$name` for functions, types and status codes (`mem$setup_paging`, `util$blkcpy_weak`, `struct lazy$block`, `lb$base_addr`, `UTIL$SUCCESS`, `KERN$PWRUP`, `LAZY$BLOCKINV`). `facility$$name` marks internal/static helpers and tests (`util$$blkcpy_weak_test`). GCC accepts `$` in identifiers; keep using it.
- Status codes follow the VMS habit of success = odd (`UTIL$SUCCESS 0x1`, `UTIL$FAIL 0x0`). Return `size_t`/`uint16_t` statuses rather than `bool`.
- Kernel log lines are prefixed `[K_INFO]`, `[K_SUCC]`, `[K_FAIL]` (`[UTIL_FAIL]` inside util) and printed with `k_print`.
- Header guards are `#ifndef __NAME__` style (some files also use `#pragma once`). Doxygen-style `@brief` comments appear on non-trivial functions.
- Indentation is mixed (tabs in `libk/tty`, `kio`, `kernel.c`; 4 spaces in `memory.c`, `vm_watchdog.c`; 2 spaces in `idt.c` gate setup). Match the file you're editing.
- Arch-specific code goes under `kernel/compat/i386/` with headers in its `include/`. Generic kernel headers sit next to their `.c` and are included with quotes.
- There is no host-side test suite. Tests are boot-time self-tests in `util$self_test()` (`libk/util/memory.c`) plus `make test`. New unit tests follow the `util$$*_test` pattern and get called from `util$self_test`.
