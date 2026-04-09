# Svanipp v1.0.0

A lightweight, command-line file transfer tool for local networks. Send files and folders between devices on the same network with integrity verification and interactive device discovery.

**Built entirely in C++ with zero external dependencies. Production-ready.**

## Features

- **Interactive Device Discovery** — Automatically discovers receivers on your network; pick from a numbered list
- **Environment Sanity Check** — `svanipp check` validates terminal and network readiness before transfers
- **Folder Recursion** — Send entire folder hierarchies while preserving directory structure
- **Multi-File Transfer** — Send multiple files and folders in a single command
- **Integrity Verification** — SHA-256 hash verification per file ensures no corruption
- **Clean Progress Output** — Real-time transfer speed and percentage tracking
- **Persistent Receiver** — Receiver accepts multiple connections sequentially, stays running
- **Windows-Native** — Built on standard C++17 with Winsock2; Linux/macOS support planned
- **No Configuration** — Works out of the box on LAN networks

## Requirements

- Windows or Linux with network connectivity
- A C++ compiler (GCC/Clang with C++17 support)
- CMake 3.20 or later

## Fresh Clone on Windows (Recommended)

If you just cloned this repository and do not have build tools installed yet, use the setup scripts.

Open PowerShell in the repository root and run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\quickstart-windows.ps1
```

What this does:
- Installs required tools (CMake + MSYS2 via `winget`)
- Installs GCC/Ninja/CMake toolchain inside MSYS2 UCRT64
- Builds `build\svanipp.exe` in Release mode
- Runs `svanipp check`

If you prefer running each step manually:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup-windows.ps1
.\scripts\build-windows.ps1 -BuildType Release
.\build\svanipp.exe check
```

Note: the first install may take several minutes.

> If you build using Visual Studio / CMake Tools instead of the repo scripts, the executable may appear under `out\build\x64-Debug\svanipp.exe`. For the scripted release flow, use `build\svanipp.exe`.

### Windows Setup

1. Install MSYS2 (includes GCC compiler)
2. Install CMake
3. Install MSYS2 UCRT64 packages: GCC, Ninja, CMake
4. Open PowerShell and navigate to the Svanipp folder

### Linux Setup

```bash
sudo apt-get install build-essential cmake
```

## Build

From the Svanipp directory:

```bash
cmake -S . -B build
cmake --build build
```

Windows script equivalent:

```powershell
.\scripts\build-windows.ps1 -BuildType Release
```

On Windows, the executable will be at `build\svanipp.exe`.  
On Linux, it will be at `build/svanipp`.

## Getting the executable quickly (Windows)

If you only want the runnable `.exe` (no developer tools), there are two simple options:

- Option A — Download a release: visit the repository's GitHub Releases page and download `svanipp-windows.zip` or `svanipp.exe` if present.
- Option B — Build & package locally (one command):

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\package-windows.ps1 -BuildType Release
```

This script will build the project (if `build\svanipp.exe` is not already present), copy the executable to `dist\svanipp.exe` and create `dist\svanipp-windows.zip`. It also copies common MSYS2 runtime DLLs when available so the release zip is more portable.

## Usage

### Start the Receiver

Run on the machine that will **receive** files:

```powershell
build\svanipp.exe receive --port 39000 --out Downloads
```

**Options:**
- `--port <number>` — Listen on this port (default: 39000)
- `--out <directory>` — Save files to this folder (default: Downloads)
- `--overwrite` — Skip confirmation and automatically overwrite existing files

The receiver will:
- Listen for incoming connections
- Display real-time progress for each transfer
- Verify SHA-256 hash for each file (silent if OK, error if mismatch)
- Accept the next connection automatically
- Print file size in MB when complete
- **Prompt before overwrite** — If a file exists, ask the user (y/n); without `--overwrite` flag

### Send Files or Folders

#### Interactive Mode (Discover and Pick Device)

```powershell
build\svanipp.exe send "C:\Users\YourName\Documents\MyFolder"
```

The program will:
1. Discover all receivers on your network
2. Show a numbered list with device names and IPs
3. Prompt you to type a number (1, 2, 3, etc.) to select one
4. Send all files to the selected device

#### Send by IP Address

```powershell
build\svanipp.exe send --ip 192.168.1.100 "C:\path\to\file.txt"
```

#### Send by Device Name

```powershell
build\svanipp.exe send --name "DESKTOP-ABC123" "C:\path\to\file.txt"
```

#### Send Multiple Files or Folders

```powershell
build\svanipp.exe send "C:\folder1" "C:\file.txt" "C:\folder2"
```

### Discover Devices

List all devices on your network that are currently running Svanipp receivers:

```powershell
build\svanipp.exe discover
```

Output example:
```
192.168.1.10  LAPTOP-XYZ  39000
192.168.1.15  DESKTOP-ABC  39000
```

### Run a Quick Readiness Check

Use this before demos or transfers to verify your environment:

```powershell
build\svanipp.exe check
```

Optional:
- `--port <number>` — Test readiness for a custom transfer port
- `--no-tui` — Show what terminal capability looks like when TUI is disabled
- `--no-color` — Force color support off in the report

The check reports:
- Local IP, hostname, and configured transfer/discovery ports
- TTY, color support, and TUI auto status
- Bind test on the transfer port (socket opens and closes immediately)
- Final verdict: `READY` or `ISSUES FOUND`

Exit code:
- `0` = ready
- non-zero = issues found

## How It Works

### Transfer Protocol

Each file transfer uses a single TCP connection with this sequence:

1. **Header** — 20 bytes with file metadata (8-byte magic, 2-byte version, 2-byte filename length, 8-byte file size)
2. **Filename** — Variable length, sent as UTF-8 bytes (may contain forward slashes for subdirectories)
3. **File Data** — Streamed in 64KB chunks
4. **SHA-256 Hash** — 32 bytes sent by sender for verification

### Folder Structure Preservation

When you send a folder like `C:\MyPhotos`:
- Folder name becomes part of the relative path
- Files inside are sent with paths like `MyPhotos\photo1.jpg`, `MyPhotos\subfolder\photo2.jpg`
- Receiver reconstructs the same structure in the output directory

Example:
```
Sender:  C:\Users\Desktop\Wallpapers\
Receiver output:  Downloads\Wallpapers\image.png
```

### Device Discovery

The receiver runs a UDP discovery responder on a dedicated port (default: 38999) that responds to broadcast queries. The sender broadcasts a discovery packet and collects responses from active receivers on the network.

## Output Examples

### Receiver Terminal

```
Listening on 0.0.0.0:39000, saving to Downloads
Receiving Wallpapers\sunset.png ... 100%
Saved: Downloads\Wallpapers\sunset.png (3.45 MB)

Receiving Documents\report.pdf ... 100%
Saved: Downloads\Documents\report.pdf (2.10 MB)
```

### Sender Terminal (Interactive Mode)

```
Discovering devices...

Available devices:
  1) LAPTOP-WORK (192.168.1.10:39000)
  2) DESKTOP-HOME (192.168.1.15:39000)

Select device (1-2): 1
Preparing to send 5 files, total size 45.67 MB

Sending Wallpapers\sunset.png
  100%
Sent: Wallpapers\sunset.png (3.45 MB)

Sending Documents\report.pdf
  100%
Sent: Documents\report.pdf (2.10 MB)

...
```

## Known Limitations

- **Port Binding** — Default port 39000 must be available; change with `--port` if it's in use. Discovery uses a separate port (38999 default).
- **Single Connection Per File** — Files are sent serially, one per TCP connection. Parallel transfers in future versions.

⚠️ **Security Warning**: Transfers are not encrypted. Files are sent in plaintext over the network. **Do not use on untrusted or public networks.**

## Architecture

```
sender.cpp       → Sends files with relative paths over TCP
receiver.cpp     → Accepts connections, reconstructs folders, verifies hashes
protocol.h       → Fixed-size header + variable name payload
socket_utils.cpp → Win32 socket helpers (send/recv with error handling)
discovery.cpp    → UDP broadcast for device discovery
crypto/sha256.cpp→ SHA-256 implementation for integrity checking
main.cpp         → CLI argument parsing and orchestration
```

## Performance

Tested on a local Gigabit network:
- Small files (< 10 MB): ~50–100 ms overhead per connection
- Large files (> 100 MB): ~90–110 MB/sec average throughput (disk-limited)
- Folder transfers: Scales linearly with file count

## Roadmap

**v1.1 (planned)**
- Resume interrupted transfers
- Bandwidth throttling

**v2.0 (future)**
- TLS encryption for secure transfers
- Parallel file transfers (multi-connection)
- GUI application
- Linux/macOS native support
- Scheduled/batched operations

## License

This project is provided as-is for personal and educational use.

## Contributing

Improvements and bug reports are welcome. Test on your network and report findings.

---

**Quick Start:**

1. Run a quick check: `build\svanipp.exe check`
2. Open two terminals
3. Terminal 1: `build\svanipp.exe receive --port 39000 --out Downloads`
4. Terminal 2: `build\svanipp.exe send "C:\some\folder"`
5. Pick the device; files transfer automatically

That's it!