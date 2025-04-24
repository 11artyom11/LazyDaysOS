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

Ensure the following tools are installed on your system:

- `make`
- `gcc`
- `nasm`
- `qemu` (for emulation)
- `gdb` (for debugging)

### Build and Run

1. **Clone the repository:**

   ```bash
   git clone https://github.com/11artyom11/LazyDaysOS.git
   cd LazyDaysOS
   ```

2. **Build the OS:**

   ```bash
   make
   ```

3. **Boot using QEMU:**

   ```bash
   ./bootit.sh
   ```

4. **Debug with GDB:**

   ```bash
   ./debug.sh
   ```

## Contribution

Contributions are welcome! Feel free to fork the repository, make changes, and submit pull requests. For major changes, please open an issue first to discuss what you would like to change.

## License

This project is open-source and available under the [MIT License](LICENSE).

---

For more details, visit the [LazyDaysOS GitHub repository](https://github.com/11artyom11/LazyDaysOS).

