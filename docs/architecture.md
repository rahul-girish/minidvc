# Architecture, Storage Format & Migration

`minidvc` stores project metadata, manifests, and content-addressed objects under `.mydvc/` at the project root.

## Local Directory Layout

```text
.mydvc/
├── project                 # Shared project metadata (tracked in Git, mode 0644)
├── credentials             # Local Google OAuth 2.0 secrets (gitignored, mode 0600)
├── manifests/              # Immutable dataset snapshot manifests
│   ├── baseline.manifest
│   └── experiment-1.manifest
├── objects/                # Content-addressable Zstandard-compressed objects (gitignored)
│   └── 68/
│       └── 2c/
│           └── 682cc703dc7cd4d9041e175d94c04fe0cacf2deb2812db5e4046a1e9a43e44d5.zst
└── tmp/                    # Staging directory for atomic writes and downloads (gitignored)
```

### Shared Metadata vs. Local Configuration (`src/project.c`)

1. **Shared Project Metadata (`.mydvc/project`)**:
   - Created by `minidvc init` and updated by `minidvc remote set`.
   - Tracked in Git so all collaborators share the same project identity and remote root folder:
     ```ini
     project_id=550e8400-e29b-41d4-a716-446655440000
     project_name=vision-classifier
     remote_folder_id=1AbCdEfGhIjKlMnOpQrStUvWxYz
     ```
   - `minidvc init` checks both the current directory and all parent directories for `.mydvc/project` to prevent accidental repeated or nested project initialization.

2. **Local Credentials (`.mydvc/credentials`)**:
   - Created by `minidvc auth` with `0600` file permissions and excluded via `.gitignore`.
   - Stores machine-specific OAuth 2.0 credentials (`client_id`, `client_secret`, `refresh_token`).

## Remote Google Drive Namespace (`src/remote.c`)

Multiple `minidvc` projects can share a single Google Drive folder without interference. All remote operations (`push`, `pull`, `list`) are scoped under the project's UUID:

```text
<remote-root>/
└── projects/
    └── <project-id>/
        ├── manifests/
        │   └── <snapshot>.manifest
        └── objects/
            └── <sha256>.zst
```

## Snapshot Manifest Format (`src/manifest.c`)

### `MINIDVC-MANIFEST 2` (Current Format)

```text
MINIDVC-MANIFEST 2
created: 1794117780
files: 3
size: 1048576
project_id: 550e8400-e29b-41d4-a716-446655440000

682cc703dc7cd4d9041e175d94c04fe0cacf2deb2812db5e4046a1e9a43e44d5	2048	data/processed/train.csv
a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447	1046016	data/raw/train.csv
84b5d26b4f1b420f465cbb78fb740dac3748178de6c4404c70c5100d2e9b7d8b	512	models/config.json
```

- **Header**: Records creation timestamp (Unix epoch seconds), file count, total uncompressed size in bytes, and project UUID.
- **Entries**: `<sha256>\t<uncompressed_bytes>\t<project_relative_path>\n`, sorted lexicographically by `<project_relative_path>`.
- **Path Safety**: Paths are normalized relative to the project root. Absolute paths, `..` traversal segments, symlinks, `.mydvc/`, and `.git/` are strictly rejected both during snapshot creation and when parsing local or remote manifests. Spaces in file and directory names are preserved.

### Checkout Behavior (`minidvc checkout <snapshot> [output-dir]`)

- By default (`minidvc checkout <snapshot>`), files are restored into the current project root (`.`), automatically creating missing parent directories (`data/raw/`, `models/`, etc.) so notebooks and training scripts work without path modifications.
- Pre-flight validation ensures that:
  1. Every manifest path is safe and inside the project root.
  2. No destination file already exists on disk (preventing silent overwrites).
  3. All required objects exist locally in `.mydvc/objects/`.
- Each file is decompressed into a sibling temporary file, verified against its SHA-256 hash, synced with `fsync()`, and atomically published via `link()`.

## Backward Compatibility & Migration

- **Legacy `MINIDVC-MANIFEST 1` Support**: Existing `MINIDVC-MANIFEST 1` files (`<sha256>\t<filename>`) remain fully readable by `checkout`, `list`, `push`, and `pull`. When listing v1 manifests locally, `minidvc` derives creation time from the manifest's modification timestamp and computes uncompressed size from local `.zst` objects.
- **Legacy `.mydvc/config` Migration**: If an older `.mydvc/config` file is present, `minidvc` automatically migrates `folder_id` into `.mydvc/project` and moves OAuth credentials into `.mydvc/credentials` (`0600`), removing the legacy `.mydvc/config` file upon saving.
