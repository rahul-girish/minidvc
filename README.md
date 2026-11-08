# minidvc

`minidvc` is a lightweight, project-aware, content-addressable data versioning tool written in C17 for machine learning operations (MLOps). Designed for small teams working across multiple ML repositories, it versions datasets using SHA-256 content addressing, Zstandard (`zstd`) streaming compression, immutable snapshots that preserve project-relative file paths, and isolated per-project namespaces on Google Drive.

## Key Features

- **Project Isolation (`minidvc init`)**: Each project is assigned a persistent UUID v4 stored in `.mydvc/project` (shared via Git). Multiple projects can safely share a single Google Drive folder under `<remote-root>/projects/<project-id>/` without snapshot or object collisions.
- **Separated Project Metadata & Local Credentials**: Shared project metadata (`project_id`, `project_name`, `remote_folder_id`) lives in `.mydvc/project`, while machine-specific OAuth 2.0 secrets live in `.mydvc/credentials` (`0600` permissions, gitignored).
- **Project-Relative Path Preservation**: `minidvc snapshot` accepts files and directories recursively and records exact project-relative paths (`data/raw/train.csv`, `notebooks/training.ipynb`). `minidvc checkout <snapshot>` restores files directly to their original locations in the project root so notebooks and scripts work without path changes.
- **Snapshot Discovery (`minidvc list`)**: Inspect available remote or local snapshots with creation timestamps, file counts, and uncompressed sizes sorted newest-first—without downloading dataset objects.
- **Crash-Safe Atomic Operations & Integrity Verification**: Every `add`, `verify`, `restore`, `push`, `pull`, and `checkout` validates decompressed SHA-256 digests and publishes files atomically via `fsync` and `link()` without silent overwrites.

---

## Installation & Setup

### Quick Install (Linux & macOS)

Run the automated installer script to install dependencies, build, and install `minidvc` to `/usr/local/bin`:

```bash
./install.sh
```

### Manual Build & Test

#### Requirements
- Compiler: `gcc` or `clang` (C17, POSIX.1-2008)
- Libraries: OpenSSL (`libcrypto`), Zstandard (`libzstd`)
- Tools: `pkg-config`, `make`, `curl`

```bash
make
make test
sudo make install
```

For platform-specific dependency installation and non-root setup, see [Installation & Multi-Machine Setup Guide](docs/installation.md).

---

## Command Reference

```text
Usage:
  minidvc init [project-name]
  minidvc snapshot <name> <file-or-directory>...
  minidvc list [--local | --remote]
  minidvc push [snapshot]
  minidvc pull [snapshot]
  minidvc checkout <snapshot> [output-dir]
  minidvc remote set <folder-id-or-url>
  minidvc remote show
  minidvc auth <client-id> <client-secret> [refresh-token]
  minidvc hash <file>
  minidvc add <file>
  minidvc verify <sha256>
  minidvc restore <sha256> <output>
  minidvc help
```

| Command | Description |
| :--- | :--- |
| `init [project-name]` | Initialize a project in the current directory with a unique UUID in `.mydvc/project`. Rejects repeated or nested initialization. |
| `snapshot <name> <path>...` | Recursively add files/directories and create an immutable manifest preserving project-relative paths. |
| `list [--local \| --remote]` | Display a table of snapshots (`NAME`, `CREATED`, `FILES`, `SIZE`) sorted newest-first. Defaults to `--remote`. |
| `push [snapshot]` | Verify and upload local snapshot(s) and objects to `<remote-root>/projects/<project-id>/`. |
| `pull [snapshot]` | Download and verify remote snapshot(s) and missing objects for the current project. |
| `checkout <snapshot> [output-dir]` | Restore snapshot files to their original project-relative paths in the project root (or inside `output-dir`). |
| `remote set <folder-id-or-url>` | Save the shared Google Drive folder ID in `.mydvc/project`. |
| `remote show` | Show project metadata, remote namespace path, and local credential status. |
| `auth <client-id> <client-secret> [refresh-token]` | Authenticate with Google Drive via OAuth 2.0 and store credentials locally in `.mydvc/credentials` (`0600`). |
| `hash <file>` | Print the 64-character hex SHA-256 digest of `<file>`. |
| `add <file>` | Compress `<file>` with Zstandard and store it in `.mydvc/objects/`. |
| `verify <sha256>` | Decompress and verify a stored object against its SHA-256 digest. |
| `restore <sha256> <output>` | Decompress, verify, and atomically publish a single object to `<output>`. |
| `help` | Display CLI help and usage information. |

---

## Two-Person Collaborative Workflow Example

### Machine A (Collaborator A — Creating & Pushing a Project Snapshot)

```bash
# 1. Initialize the project and configure the shared Google Drive folder
minidvc init
minidvc remote set "https://drive.google.com/drive/folders/1AbCdEfGhIjKlMnOpQrStUvWxYz"
minidvc auth "<CLIENT_ID>" "<CLIENT_SECRET>"

# 2. Snapshot dataset directories and notebooks, then push to Google Drive
minidvc snapshot experiment-1 data/ notebooks/
minidvc push experiment-1

# 3. Commit shared project metadata (.mydvc/project) and code to Git
git add .mydvc/project .gitignore
git commit -m "Initialize minidvc project"
git push
```

### Machine B (Collaborator B — Discovering, Pulling & Checking Out)

```bash
# 1. Clone the project repository (includes .mydvc/project with project_id & remote_folder_id)
git clone <repo-url>
cd <repo-dir>

# 2. Authenticate locally on Machine B
minidvc auth "<CLIENT_ID>" "<CLIENT_SECRET>"

# 3. Discover available remote snapshots without downloading dataset objects
minidvc list --remote

# 4. Pull and restore experiment-1 into the project root
minidvc pull experiment-1
minidvc checkout experiment-1
```

All files in `data/` and `notebooks/` are recreated at their exact relative paths inside Machine B's project root.

---

## Additional Documentation

- [Installation & Multi-Machine Setup Guide](docs/installation.md)
- [Architecture, Storage Format & Migration](docs/architecture.md)
- [Google Drive Collaboration & Permissions Guide](docs/google-drive-collaboration.md)
