# Installation & Multi-Machine Setup Guide

This guide covers installing `minidvc` across Linux, macOS, WSL2, and headless servers, and onboarding additional machines onto an existing project.

---

## 1. Quick Automated Installation

On Linux or macOS, run [`install.sh`](../install.sh) to install system dependencies, compile `minidvc`, and install the binary to `/usr/local/bin` (or `~/.local/bin` if running without root privileges):

```bash
git clone <minidvc-repo-url> minidvc
cd minidvc
./install.sh
```

---

## 2. Manual Installation by Platform

### Debian / Ubuntu / Pop!_OS / Linux Mint
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libssl-dev libzstd-dev curl

make
make test
sudo make install
```

### Fedora / RHEL / CentOS / Rocky Linux
```bash
sudo dnf install -y gcc gcc-c++ make pkgconfig openssl-devel libzstd-devel curl

make
make test
sudo make install
```

### Arch Linux / Manjaro
```bash
sudo pacman -S --noconfirm base-devel openssl zstd curl pkgconf

make
make test
sudo make install
```

### macOS (Homebrew)
```bash
brew install make pkg-config openssl@3 zstd curl
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

make
make test
sudo make install
```

### User-Local Installation (No Root / Sudo)
```bash
make
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

---

## 3. Onboarding a New Collaborator Machine

When cloning an existing ML repository that already contains `.mydvc/project`:

```bash
# 1. Authenticate on the new machine (saves OAuth tokens to .mydvc/credentials with 0600 permissions)
minidvc auth "<CLIENT_ID>" "<CLIENT_SECRET>"

# 2. View project and remote status
minidvc remote show

# 3. List available remote snapshots
minidvc list

# 4. Pull and checkout a snapshot into the project root
minidvc pull <snapshot-name>
minidvc checkout <snapshot-name>
```

---

## 4. Headless Servers & CI/CD Environments

On headless machines or CI runners, supply credentials via a refresh token or environment variables:

```bash
export MINIDVC_GDRIVE_CLIENT_ID="<CLIENT_ID>"
export MINIDVC_GDRIVE_CLIENT_SECRET="<CLIENT_SECRET>"
export MINIDVC_GDRIVE_REFRESH_TOKEN="<REFRESH_TOKEN>"
# Optional if not already set in .mydvc/project:
export MINIDVC_GDRIVE_FOLDER_ID="<FOLDER_ID>"

minidvc list --remote
minidvc pull <snapshot-name>
minidvc checkout <snapshot-name>
```
