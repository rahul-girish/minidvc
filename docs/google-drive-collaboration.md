# Google Drive Collaboration & Permissions Guide

`minidvc` enables teams to collaborate across multiple machine-learning projects using a single shared Google Drive folder. Each project is isolated under `<remote-root>/projects/<project-id>/`.

---

## 1. Google Cloud OAuth 2.0 Setup

Create an OAuth 2.0 Client ID in the Google Cloud Console (one-time setup per team):

1. Open the [Google Cloud Console](https://console.cloud.google.com/).
2. Create or select a project and enable the **Google Drive API** under **APIs & Services > Library**.
3. Under **APIs & Services > OAuth consent screen**:
   - Configure the consent screen and add the scope `https://www.googleapis.com/auth/drive`.
   - If the app is in "Testing" mode, add your collaborators' Google accounts under **Test users**.
4. Under **APIs & Services > Credentials**:
   - Click **Create Credentials > OAuth client ID** and choose **Desktop app**.
   - Share the **Client ID** and **Client Secret** with your teammates.

---

## 2. Shared Google Drive Folder Permissions

1. Create a shared folder in Google Drive (or a Google Workspace Shared Drive).
2. Share the folder with your collaborators:
   - **Pushing (`minidvc push`)**: Requires **Editor** (My Drive) or **Contributor / Content Manager** (Shared Drive). `minidvc` checks `capabilities.canAddChildren` before uploading.
   - **Listing & Pulling (`minidvc list`, `minidvc pull`)**: Requires **Viewer** (or higher) access (`capabilities.canListChildren`).

Multiple `minidvc` projects can point to the exact same shared Google Drive folder—each project stores its manifests and objects inside `projects/<project-id>/`.

---

## 3. Complete Two-Person Collaboration Walkthrough

### Collaborator A: Initialize Project, Snapshot & Push

```bash
# 1. Initialize the project in the repository root
minidvc init

# 2. Set the shared Google Drive folder (stored in .mydvc/project)
minidvc remote set "https://drive.google.com/drive/folders/1AbCdEfGhIjKlMnOpQrStUvWxYz"

# 3. Authenticate with Google Drive (stored locally in .mydvc/credentials with mode 0600)
minidvc auth "<CLIENT_ID>" "<CLIENT_SECRET>"

# 4. Snapshot directories and files (preserves nested relative paths)
minidvc snapshot baseline data/raw data/processed models/config.json

# 5. Push snapshot to Google Drive
minidvc push baseline

# 6. Commit .mydvc/project to Git so collaborators share the project ID and remote folder
git add .mydvc/project .gitignore
git commit -m "Track minidvc project metadata"
git push
```

### Collaborator B: Clone, List, Pull & Checkout

```bash
# 1. Clone the Git repository
git clone <repo-url>
cd <repo-dir>

# 2. Authenticate locally on Collaborator B's machine
minidvc auth "<CLIENT_ID>" "<CLIENT_SECRET>"

# 3. List available snapshots on the remote (does not download dataset objects)
minidvc list
# Output:
# NAME              CREATED             FILES      SIZE
# ------------------------------------------------------------
# baseline          2026-11-08 06:10    3          14.2 MB

# 4. Pull the desired snapshot
minidvc pull baseline

# 5. Restore files to their original project-relative locations
minidvc checkout baseline
```

---

## 4. Discovering Snapshots (`minidvc list`)

- `minidvc list` or `minidvc list --remote`: Queries the project's remote namespace on Google Drive and displays available snapshots sorted newest-first without downloading `.zst` dataset objects.
- `minidvc list --local`: Lists snapshots available in `.mydvc/manifests/` offline without requiring network access.
