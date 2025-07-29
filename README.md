# Interval Training Timer

A high-contrast, full-screen interval training timer written in C for Linux systems.

### System Dependencies

You'll need to install the following development libraries on your system:

**For Manjaro/Arch Linux:**
```bash
sudo pacman -S gcc make libx11 libxext alsa-lib cairo libxrandr
```

### Compilation

```bash
make
```

This will create the `interval_timer` executable.

## Usage

```bash
./interval_timer strength_intervals.txt
```