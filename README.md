# LazyDaysOS

**LazyDaysOS** (also referred to as *DudeOS*) is a hobbyist operating system built from scratch using C and Assembly. It serves as an educational project to explore OS development fundamentals, including kernel design, low-level memory management, and bootloader integration.

## Features

- Custom kernel written in C and Assembly
- Multiboot-compliant bootloader support
- Basic shell scripts for building and debugging
- Modular codebase with components like `libk` and `kernel`
- GRUB configuration generation

## Project Structure

```
LazyDaysOS/
├── .vscode/             # VSCode configuration files
├── kernel/              # Core kernel source code
├── libk/                # Kernel library code
├── bootit.sh            # Script to build and boot the OS
├── debug.sh             # Script to debug using GDB
├── gen_grubcfg.sh       # Generates GRUB configuration
├── test_multiboot.sh    # Tests Multiboot compliance
├── gdbconf              # GDB configuration file
├── Makefile             # Build system
├── .gitignore
└── README.md
```

## Getting Started

### Prerequisites

- An **`i686-elf` cross compiler** (`i686-elf-gcc`, `i686-elf-as`, `i686-elf-ar`). The host `gcc` will not work: the kernel refuses to build with a compiler that targets Linux. See the [OSDev wiki: GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler), or the recipe below.
- `make`
- GRUB with BIOS modules, `xorriso` and `mtools` (for `grub-mkrescue` / `grub-file`)
- `qemu-system-i386` (for emulation)
- `gdb` (for debugging), and `gnome-terminal` if you use `debug.sh`

On Ubuntu/Debian:

```bash
sudo apt install build-essential make grub-pc-bin grub-common xorriso mtools qemu-system-x86 gdb
```

#### Building the cross compiler

Install the toolchain into `~/opt/cross` (the path `.vscode/c_cpp_properties.json` expects). This builds GMP/MPFR/MPC in-tree, so no extra `-dev` packages are needed:

```bash
export PREFIX="$HOME/opt/cross" TARGET=i686-elf PATH="$HOME/opt/cross/bin:$PATH"
mkdir -p ~/opt/cross-build && cd ~/opt/cross-build
wget https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-15.3.0/gcc-15.3.0.tar.xz
tar xf binutils-2.47.tar.xz && tar xf gcc-15.3.0.tar.xz
(cd gcc-15.3.0 && ./contrib/download_prerequisites)

mkdir build-binutils && cd build-binutils
../binutils-2.47/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror MAKEINFO=true
make -j$(nproc) MAKEINFO=true && make install MAKEINFO=true && cd ..

mkdir build-gcc && cd build-gcc
../gcc-15.3.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c --without-headers --disable-hosted-libstdcxx MAKEINFO=true
make -j$(nproc) all-gcc all-target-libgcc MAKEINFO=true
make install-gcc install-target-libgcc MAKEINFO=true
```

Then add `export PATH="$HOME/opt/cross/bin:$PATH"` to your `~/.bashrc`.

### Build and Run

1. **Clone the repository:**

   ```bash
   git clone https://github.com/11artyom11/LazyDaysOS.git
   cd LazyDaysOS
   ```

2. **Build the OS:**

   ```bash
   make install
   ```

   This builds `LazyDaysOS.bin`, stages it into `sysroot/`, creates `sysroot/boot/kernel/LazyDaysOS.iso` and checks that the kernel is a valid multiboot image. (Plain `make` only builds the kernel binary in the repo root; the boot scripts read from `sysroot/`.)

3. **Run the self tests:**

   ```bash
   make check
   ```

   This runs two stages, both headless in QEMU and without root:

   - **Self tests:** the kernel runs its built-in tests (memory allocators, interrupts, timer, checksums, and pinging the gateways of two emulated Intel e1000 network cards) and reports the result back to QEMU. Serial log: `qemu_check.log`.
   - **Router tests against Linux:** the kernel runs as a router between two private network namespaces (a LAN host and a WAN host), and real Linux tools check it as a host and as a router: pings both ways through it, static routes, ICMP error messages, UDP and loss-free forwarding bursts, then, in a "home router" scenario, its DHCP client against `dnsmasq` upstream, its DHCP server against `udhcpc` on the LAN, and NAT with its firewall (pings, UDP and a 1 MiB TCP transfer from the LAN to a WAN that has no route back, unsolicited traffic blocked); and finally the kernel's own TCP server, including transfers with 10% packet loss injected in both directions. `tcpdump` verifies every checksum in the captures.

4. **Boot using QEMU:**

   ```bash
   ./bootit.sh          # boot the kernel directly (qemu -kernel)
   ./bootit.sh --iso    # boot the GRUB ISO
   ```

   Kernel messages appear both in the QEMU window and, through the serial port, in the terminal you started it from. With `-kernel`, the network card is configured as `10.0.2.15/24` with gateway `10.0.2.2` (QEMU's user networking).

5. **Debug with GDB:**

   ```bash
   ./debug.sh
   ```

   This opens two terminals: QEMU halted and waiting for a debugger (`./bootit.sh -d`), and GDB using `gdbconf`, which connects to `localhost:1234` and breaks at `_start`.

6. **Clean:**

   ```bash
   make clean
   ```

## Contribution

Contributions are welcome! Feel free to fork the repository, make changes, and submit pull requests. For major changes, please open an issue first to discuss what you would like to change.

## License

This project is open-source and available under the [MIT License](LICENSE).

---

For more details, visit the [LazyDaysOS GitHub repository](https://github.com/11artyom11/LazyDaysOS).

