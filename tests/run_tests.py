#!/usr/bin/env python3
"""
Comprehensive integration test suite for minidvc.
Covers project initialization, UUID isolation, manifest v1/v2 compatibility,
nested path preservation on checkout, spaces in filenames, path traversal
rejection, local/remote listing, selective pull, deduplication, corruption
detection, and multi-project Google Drive namespace isolation.
"""

import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import tempfile
import threading
import time
import urllib.parse

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINIDVC_BIN = os.path.join(REPO_ROOT, "minidvc")

UUID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
)


class MockGDriveState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.can_add_children = True
        self.folders = {
            "shared_root_folder": {
                "id": "shared_root_folder",
                "name": "Team-MLOps-Drive",
                "mimeType": "application/vnd.google-apps.folder",
                "trashed": False,
                "parents": [],
            }
        }
        self.files = {}
        self.uploads = {}
        self.next_id = 100
        self.object_download_count = 0
        self.manifest_download_count = 0
        self.corrupt_next_download = False

    def alloc_id(self):
        self.next_id += 1
        return f"gd_{self.next_id}"


STATE = MockGDriveState()


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


class MockGDriveHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)

        if parsed.path == "/token":
            params = urllib.parse.parse_qs(body.decode("utf-8"))
            if (
                params.get("client_id") == ["cid_valid"]
                and params.get("client_secret") == ["csec_valid"]
                and params.get("refresh_token") == ["rtok_valid"]
            ):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(
                    json.dumps({"access_token": "tok_valid"}).encode("utf-8")
                )
            else:
                self.send_response(401)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(
                    json.dumps({"error_description": "Invalid OAuth credentials"}).encode("utf-8")
                )
            return

        if self.headers.get("Authorization") != "Bearer tok_valid":
            self.send_response(401)
            self.end_headers()
            return

        if parsed.path == "/drive/v3/files":
            meta = json.loads(body.decode("utf-8"))
            fid = STATE.alloc_id()
            STATE.folders[fid] = {
                "id": fid,
                "name": meta["name"],
                "mimeType": meta["mimeType"],
                "trashed": False,
                "parents": meta.get("parents", []),
            }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"id": fid}).encode("utf-8"))
            return

        if parsed.path == "/upload/drive/v3/files":
            meta = json.loads(body.decode("utf-8"))
            uid = STATE.alloc_id()
            STATE.uploads[uid] = meta
            port = self.server.server_address[1]
            self.send_response(200)
            self.send_header("Location", f"http://127.0.0.1:{port}/upload/{uid}")
            self.end_headers()
            return

        self.send_response(404)
        self.end_headers()

    def do_PUT(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path.startswith("/upload/"):
            uid = parsed.path.split("/")[-1]
            meta = STATE.uploads.pop(uid)
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length)
            fid = STATE.alloc_id()
            STATE.files[fid] = {
                "id": fid,
                "name": meta["name"],
                "parents": meta.get("parents", []),
                "trashed": False,
                "content": data,
            }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(
                json.dumps({"id": fid, "name": meta["name"]}).encode("utf-8")
            )
            return
        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        if self.headers.get("Authorization") != "Bearer tok_valid":
            self.send_response(401)
            self.end_headers()
            return

        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)

        if parsed.path.startswith("/drive/v3/files/"):
            fid = parsed.path.split("/")[-1]
            if qs.get("alt") == ["media"]:
                if fid in STATE.files:
                    fobj = STATE.files[fid]
                    content = fobj["content"]
                    if fobj["name"].endswith(".zst"):
                        STATE.object_download_count += 1
                        if STATE.corrupt_next_download:
                            content = b"corrupted-zst-payload"
                    elif fobj["name"].endswith(".manifest"):
                        STATE.manifest_download_count += 1
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(content)))
                    self.end_headers()
                    self.wfile.write(content)
                    return
                self.send_response(404)
                self.end_headers()
                return

            if fid in STATE.folders:
                f = STATE.folders[fid]
                resp = {
                    "id": f["id"],
                    "name": f["name"],
                    "mimeType": f["mimeType"],
                    "trashed": f["trashed"],
                    "capabilities": {
                        "canAddChildren": STATE.can_add_children,
                        "canListChildren": True,
                    },
                }
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(resp).encode("utf-8"))
                return

            self.send_response(404)
            self.end_headers()
            return

        if parsed.path == "/drive/v3/files":
            q = qs.get("q", [""])[0]
            parent_id = q.split("'")[1]
            want_folder = "mimeType='application/vnd.google-apps.folder'" in q
            result = []
            if want_folder:
                name_part = q.split("name='")[1].split("'")[0]
                for f in STATE.folders.values():
                    if parent_id in f["parents"] and f["name"] == name_part and not f["trashed"]:
                        result.append({"id": f["id"], "name": f["name"]})
            else:
                for f in STATE.files.values():
                    if parent_id in f["parents"] and not f["trashed"]:
                        result.append({"id": f["id"], "name": f["name"]})

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"files": result}).encode("utf-8"))
            return

        self.send_response(404)
        self.end_headers()


def run(args, cwd, env):
    return subprocess.run(
        [MINIDVC_BIN] + args, cwd=cwd, env=env, capture_output=True, text=True
    )


def parse_project_file(path):
    data = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                data[k] = v
    return data


def write_file(base, relpath, content):
    full = os.path.join(base, relpath)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    mode = "wb" if isinstance(content, bytes) else "w"
    with open(full, mode) as f:
        f.write(content)
    return full


def run_all_tests():
    server = ReusableTCPServer(("127.0.0.1", 0), MockGDriveHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    base_env = os.environ.copy()
    base_env["MINIDVC_GDRIVE_API_BASE"] = f"http://127.0.0.1:{port}"
    base_env["MINIDVC_GDRIVE_OAUTH_TOKEN_URL"] = f"http://127.0.0.1:{port}/token"
    for k in [
        "MINIDVC_GDRIVE_FOLDER_ID",
        "MINIDVC_GDRIVE_CLIENT_ID",
        "MINIDVC_GDRIVE_CLIENT_SECRET",
        "MINIDVC_GDRIVE_REFRESH_TOKEN",
    ]:
        base_env.pop(k, None)

    tmp_root = tempfile.mkdtemp(prefix="minidvc_tests_")

    try:
        # ------------------------------------------------------------------
        # Test 1, 2, 3: Project init, repeated/nested init rejection, UUIDs
        # ------------------------------------------------------------------
        proj1_a = os.path.join(tmp_root, "proj1_machine_a")
        proj2 = os.path.join(tmp_root, "proj2")
        os.makedirs(proj1_a)
        os.makedirs(proj2)

        r = run(["init", "vision-classifier"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        p1_meta = parse_project_file(os.path.join(proj1_a, ".mydvc/project"))
        assert p1_meta["project_name"] == "vision-classifier"
        assert UUID_RE.match(p1_meta["project_id"]), p1_meta["project_id"]

        # Reject repeated init in the same directory
        r = run(["init"], proj1_a, base_env)
        assert r.returncode != 0
        assert "already initialized" in r.stderr

        # Reject nested project initialization inside a subdirectory
        nested_dir = os.path.join(proj1_a, "notebooks/subfolder")
        os.makedirs(nested_dir)
        r = run(["init"], nested_dir, base_env)
        assert r.returncode != 0
        assert "nested project" in r.stderr

        # Second project gets a distinct UUID
        r = run(["init", "nlp-summarizer"], proj2, base_env)
        assert r.returncode == 0, r.stderr
        p2_meta = parse_project_file(os.path.join(proj2, ".mydvc/project"))
        assert UUID_RE.match(p2_meta["project_id"])
        assert p1_meta["project_id"] != p2_meta["project_id"]
        print("[PASS] 1-3: Project initialization, nested/repeat rejection, distinct UUIDs")

        # ------------------------------------------------------------------
        # Test 4, 7, 8, 9: Nested directories, spaces in filenames,
        #                  path traversal rejection, duplicate handling
        # ------------------------------------------------------------------
        write_file(proj1_a, "notebooks/training.ipynb", '{"cells": []}\n')
        write_file(proj1_a, "data/raw/train.csv", "id,x,y\n1,10,20\n2,30,40\n")
        write_file(proj1_a, "data/processed/train.csv", "id,norm_x,y\n1,0.1,20\n")
        write_file(proj1_a, "data/raw/holdout set/test split.csv", "id,x\n9,99\n")
        write_file(proj1_a, "models/config.json", '{"lr": 0.001}\n')

        # Reject path traversal on snapshot CLI
        outside_file = write_file(tmp_root, "outside.txt", "secret")
        r = run(["snapshot", "bad1", "../outside.txt"], proj1_a, base_env)
        assert r.returncode != 0
        r = run(["snapshot", "bad2", outside_file], proj1_a, base_env)
        assert r.returncode != 0

        # Reject missing files before creating snapshot
        r = run(["snapshot", "bad3", "data/raw/train.csv", "nonexistent.csv"], proj1_a, base_env)
        assert r.returncode != 0
        assert not os.path.exists(os.path.join(proj1_a, ".mydvc/manifests/bad3.manifest"))

        # Snapshot directories + duplicate file argument safely deduplicated
        r = run(
            [
                "snapshot",
                "baseline",
                "data",
                "notebooks",
                "models/config.json",
                "./data/raw/train.csv",
            ],
            proj1_a,
            base_env,
        )
        assert r.returncode == 0, r.stderr
        assert "5 files" in r.stdout

        # Reject duplicate snapshot name
        r = run(["snapshot", "baseline", "models/config.json"], proj1_a, base_env)
        assert r.returncode != 0
        assert "already exists" in r.stderr
        print("[PASS] 4, 7, 8, 9: Nested dirs, spaces, traversal rejection, duplicate handling")

        # ------------------------------------------------------------------
        # Test 5: Restoring files to exact original relative paths + no overwrite
        # ------------------------------------------------------------------
        # Attempting checkout when files already exist must refuse to overwrite
        r = run(["checkout", "baseline"], proj1_a, base_env)
        assert r.returncode != 0
        assert "refusing to overwrite" in r.stderr

        # Remove working tree files and directories, then run `minidvc checkout baseline`
        shutil.rmtree(os.path.join(proj1_a, "data"))
        shutil.rmtree(os.path.join(proj1_a, "notebooks"))
        shutil.rmtree(os.path.join(proj1_a, "models"))

        r = run(["checkout", "baseline"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        assert os.path.isfile(os.path.join(proj1_a, "data/raw/train.csv"))
        assert os.path.isfile(os.path.join(proj1_a, "data/processed/train.csv"))
        assert os.path.isfile(os.path.join(proj1_a, "data/raw/holdout set/test split.csv"))
        assert os.path.isfile(os.path.join(proj1_a, "notebooks/training.ipynb"))
        assert os.path.isfile(os.path.join(proj1_a, "models/config.json"))
        with open(os.path.join(proj1_a, "data/raw/train.csv")) as f:
            assert f.read() == "id,x,y\n1,10,20\n2,30,40\n"
        with open(os.path.join(proj1_a, "data/processed/train.csv")) as f:
            assert f.read() == "id,norm_x,y\n1,0.1,20\n"
        print("[PASS] 5: Checkout restores exact nested project-relative paths without overwriting")

        # ------------------------------------------------------------------
        # Test 10: Local snapshot listing (sorted newest first, works offline)
        # ------------------------------------------------------------------
        time.sleep(1.1)
        write_file(proj1_a, "data/augmented.csv", "a,b,c\n" * 100)
        r = run(["snapshot", "experiment-1", "data", "notebooks"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr

        r = run(["list", "--local"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        lines = [ln.strip() for ln in r.stdout.strip().splitlines() if ln.strip()]
        assert "NAME" in lines[0] and "CREATED" in lines[0] and "FILES" in lines[0] and "SIZE" in lines[0]
        # experiment-1 was created after baseline, so it must appear before baseline
        idx_exp1 = next(i for i, ln in enumerate(lines) if ln.startswith("experiment-1"))
        idx_base = next(i for i, ln in enumerate(lines) if ln.startswith("baseline"))
        assert idx_exp1 < idx_base, f"Expected newest first, got:\n{r.stdout}"
        print("[PASS] 10: Local snapshot listing sorted newest-first")

        # ------------------------------------------------------------------
        # Test 11, 12: Empty remote handling, remote access error distinction,
        #              pushing, and remote listing without object downloads
        # ------------------------------------------------------------------
        r = run(["remote", "set", "https://drive.google.com/drive/folders/shared_root_folder"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        r = run(["auth", "cid_valid", "csec_valid", "rtok_valid"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr

        # Verify credentials are in .mydvc/credentials (0600) and folder_id is in .mydvc/project
        cred_path = os.path.join(proj1_a, ".mydvc/credentials")
        assert os.path.exists(cred_path)
        assert oct(os.stat(cred_path).st_mode & 0o777) == "0o600"
        p1_meta = parse_project_file(os.path.join(proj1_a, ".mydvc/project"))
        assert p1_meta["remote_folder_id"] == "shared_root_folder"
        with open(os.path.join(proj1_a, ".mydvc/project")) as f:
            proj_content = f.read()
            assert "csec_valid" not in proj_content and "rtok_valid" not in proj_content

        # Empty remote before any push
        r = run(["list", "--remote"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        assert "(no snapshots found)" in r.stdout

        # Push baseline and experiment-1
        r = run(["push"], proj1_a, base_env)
        assert r.returncode == 0, r.stderr
        assert "2 manifest(s) uploaded" in r.stdout
        print("[PASS] 11-12 (part 1): Empty remote handling and push all snapshots")

        # ------------------------------------------------------------------
        # Test 6, 11, 14, 15: Collaborator B on another machine clones project,
        #                     lists remote without downloading objects,
        #                     pulls single snapshot, avoids redundant downloads,
        #                     and checks out to project root
        # ------------------------------------------------------------------
        proj1_b = os.path.join(tmp_root, "proj1_machine_b")
        os.makedirs(os.path.join(proj1_b, ".mydvc"))
        # Simulate Git clone of tracked project metadata (.mydvc/project)
        shutil.copy2(
            os.path.join(proj1_a, ".mydvc/project"),
            os.path.join(proj1_b, ".mydvc/project"),
        )

        r = run(["auth", "cid_valid", "csec_valid", "rtok_valid"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr

        STATE.object_download_count = 0
        r = run(["list", "--remote"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr
        assert "experiment-1" in r.stdout and "baseline" in r.stdout
        # Must NOT download any .zst objects during `minidvc list`
        assert STATE.object_download_count == 0, f"Downloaded {STATE.object_download_count} objects during list!"

        # Pull only `baseline` (5 objects, not `augmented.csv` from experiment-1)
        r = run(["pull", "baseline"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr
        assert "5 object(s), 1 manifest(s) downloaded" in r.stdout
        assert STATE.object_download_count == 5

        # Pulling `baseline` a second time must download 0 objects
        STATE.object_download_count = 0
        r = run(["pull", "baseline"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr
        assert STATE.object_download_count == 0

        # Pulling `experiment-1` should only download the 1 new object (augmented.csv)
        r = run(["pull", "experiment-1"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr
        assert STATE.object_download_count == 1

        # Checkout `experiment-1` on Machine B into project root
        r = run(["checkout", "experiment-1"], proj1_b, base_env)
        assert r.returncode == 0, r.stderr
        assert os.path.isfile(os.path.join(proj1_b, "data/raw/train.csv"))
        assert os.path.isfile(os.path.join(proj1_b, "data/processed/train.csv"))
        assert os.path.isfile(os.path.join(proj1_b, "data/raw/holdout set/test split.csv"))
        assert os.path.isfile(os.path.join(proj1_b, "data/augmented.csv"))
        assert os.path.isfile(os.path.join(proj1_b, "notebooks/training.ipynb"))
        print("[PASS] 6, 11, 14, 15: Machine B remote list, selective pull, deduplication, and root checkout")

        # ------------------------------------------------------------------
        # Test 13: Multi-project isolation on the same Google Drive folder
        # ------------------------------------------------------------------
        r = run(["remote", "set", "shared_root_folder"], proj2, base_env)
        assert r.returncode == 0, r.stderr
        r = run(["auth", "cid_valid", "csec_valid", "rtok_valid"], proj2, base_env)
        assert r.returncode == 0, r.stderr

        # Project 2 must not see Project 1's snapshots!
        r = run(["list", "--remote"], proj2, base_env)
        assert r.returncode == 0, r.stderr
        assert "(no snapshots found)" in r.stdout
        assert "baseline" not in r.stdout and "experiment-1" not in r.stdout

        # Project 2 creates its own `baseline` snapshot and pushes it
        write_file(proj2, "corpus/tokens.txt", "hello world nlp\n")
        r = run(["snapshot", "baseline", "corpus"], proj2, base_env)
        assert r.returncode == 0, r.stderr
        r = run(["push", "baseline"], proj2, base_env)
        assert r.returncode == 0, r.stderr

        # Project 2 sees only its 1-file baseline, while Project 1 still sees its 5-file baseline
        r2 = run(["list", "--remote"], proj2, base_env)
        assert r2.returncode == 0
        assert "baseline" in r2.stdout and "experiment-1" not in r2.stdout

        r1 = run(["list", "--remote"], proj1_b, base_env)
        assert r1.returncode == 0
        assert "experiment-1" in r1.stdout and "baseline" in r1.stdout
        print("[PASS] 13: Two projects sharing one Google Drive folder are completely isolated")

        # ------------------------------------------------------------------
        # Test 8 (part 2) & 16: Rejecting malicious remote manifest paths &
        #                       detecting corrupted objects
        # ------------------------------------------------------------------
        # Inject a tampered manifest on the remote with "../escaped.txt"
        for fid, fobj in list(STATE.files.items()):
            if fobj["name"] == "baseline.manifest" and len(fobj["content"]) < 300:
                STATE.files["tampered_id"] = {
                    "id": "tampered_id",
                    "name": "evil.manifest",
                    "parents": fobj["parents"],
                    "trashed": False,
                    "content": (
                        b"MINIDVC-MANIFEST 2\n"
                        b"created: 1700000000\n"
                        b"files: 1\n"
                        b"size: 16\n\n"
                        b"a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447\t16\t../evil.txt\n"
                    ),
                }
                break

        r = run(["pull", "evil"], proj2, base_env)
        assert r.returncode != 0
        assert "invalid manifest" in r.stderr

        # Test corrupted object download detection on pull
        proj2_clone = os.path.join(tmp_root, "proj2_clone")
        os.makedirs(os.path.join(proj2_clone, ".mydvc"))
        shutil.copy2(
            os.path.join(proj2, ".mydvc/project"),
            os.path.join(proj2_clone, ".mydvc/project"),
        )
        run(["auth", "cid_valid", "csec_valid", "rtok_valid"], proj2_clone, base_env)
        STATE.corrupt_next_download = True
        r = run(["pull", "baseline"], proj2_clone, base_env)
        STATE.corrupt_next_download = False
        assert r.returncode != 0
        assert "failed verification" in r.stderr
        print("[PASS] 8 & 16: Malicious manifest path traversal and corrupted object detection")

        # ------------------------------------------------------------------
        # Test 17: Backward compatibility with MINIDVC-MANIFEST 1
        # ------------------------------------------------------------------
        legacy_dir = os.path.join(tmp_root, "legacy_proj")
        os.makedirs(legacy_dir)
        write_file(legacy_dir, "legacy.csv", "col1,col2\n1,2\n")
        r = run(["add", "legacy.csv"], legacy_dir, base_env)
        assert r.returncode == 0, r.stderr
        r_hash = run(["hash", "legacy.csv"], legacy_dir, base_env)
        legacy_hash = r_hash.stdout.strip()

        write_file(
            legacy_dir,
            ".mydvc/manifests/old_v1.manifest",
            f"MINIDVC-MANIFEST 1\n{legacy_hash}\tlegacy.csv\n",
        )
        os.remove(os.path.join(legacy_dir, "legacy.csv"))

        r = run(["list", "--local"], legacy_dir, base_env)
        assert r.returncode == 0, r.stderr
        assert "old_v1" in r.stdout and "14 B" in r.stdout

        r = run(["checkout", "old_v1"], legacy_dir, base_env)
        assert r.returncode == 0, r.stderr
        assert os.path.isfile(os.path.join(legacy_dir, "legacy.csv"))
        print("[PASS] 17: Backward compatibility with MINIDVC-MANIFEST 1 manifests")

        print("\nALL 17 TEST SUITES PASSED SUCCESSFULLY!")
    finally:
        server.shutdown()
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    run_all_tests()
