#!/usr/bin/env python3
"""Publish this project to GitHub and start the 11u low-voltage kernel build.

Why the REST API instead of `git push`:
  On some networks only api.github.com is reachable; github.com (git smart HTTP
  and the OAuth device endpoint) is blocked by the local proxy.  The Git Data
  API can create blobs/trees/commits/refs over api.github.com alone, so the
  whole publish + dispatch flow works without the git transport.

Usage:
  export GH_TOKEN=ghp_xxx            # classic PAT with `repo` + `workflow`
  python tools/gh_build.py                      # create repo, push, trigger build
  python tools/gh_build.py --repo my-name --private
  python tools/gh_build.py --follow             # also wait for the run result
  python tools/gh_build.py --status             # just report the latest run

Token lookup order: --token, GH_TOKEN / GITHUB_TOKEN, --token-file, then
.gh_token in the project root or the home directory (first line, stripped).
The token is only sent as an Authorization header to api.github.com; it is
never persisted by this script and never written into .git/config.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

API = "https://api.github.com"
ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = "build-11u-lowvolt.yml"
BRANCH = "main"

# Directories/files that must never be uploaded to the build repo.
SKIP_DIRS = {".git", "research", "__pycache__", "artifacts", "kernel", ".github"}
SKIP_DIR_PREFIXES = ("out-star-",)
SKIP_SUFFIXES = (".pyc", ".zip", ".gz", ".tar", ".xz", ".pem", ".key")
SKIP_NAMES = {
    "Image.gz",
    # Never publish local credentials, even if someone drops them in the repo.
    ".gh_token", ".env", ".netrc", ".npmrc", ".pypirc",
    "id_rsa", "id_ed25519",
}
SENSITIVE_NAME_MARKERS = ("token", "secret", "credential", "password", "private_key")

# .github/workflows is inside SKIP_DIRS above for the generic walk, but we
# explicitly add it back below because the workflow file must be published.
SPECIAL_FILES = (Path(".github/workflows") / WORKFLOW,)

DEFAULT_INPUTS = {
    "upstream_ref": "venus-5.10bpf",
    "shutdown_mv": "2980",
    "volt_soc_table": "",
    "clang_url": "",
    "llvm_ias": "1",
    "jobs": "4",
}


class GitHubError(RuntimeError):
    def __init__(self, method: str, path: str, status: int, body: str):
        super().__init__(f"{method} {path} -> HTTP {status}: {body[:600]}")
        self.status = status


def request(method: str, path: str, token: str, body=None):
    url = path if path.startswith("http") else API + path
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "Authorization": f"Bearer {token}",
        "User-Agent": "star-11u-lowvolt-builder",
    }
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=90) as resp:
            raw = resp.read()
            if not raw:
                return None, resp.headers
            return json.loads(raw.decode("utf-8")), resp.headers
    except urllib.error.HTTPError as exc:
        raise GitHubError(method, path, exc.code, exc.read().decode("utf-8", "replace")) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"network error calling {path}: {exc.reason}") from exc


def collect_files() -> list[tuple[str, bytes]]:
    """Return (repo-relative posix path, bytes) for everything to publish."""
    out: dict[str, bytes] = {}
    for special in SPECIAL_FILES:
        full = ROOT / special
        if not full.is_file():
            raise RuntimeError(f"missing required file: {full}")
        out[special.as_posix()] = full.read_bytes()
    for full in sorted(ROOT.rglob("*")):
        if not full.is_file():
            continue
        rel = full.relative_to(ROOT)
        parts = rel.parts
        if any(p in SKIP_DIRS for p in parts[:-1]):
            continue
        if any(p.startswith(SKIP_DIR_PREFIXES) for p in parts[:-1]):
            continue
        lower_name = rel.name.lower()
        if lower_name in SKIP_NAMES or rel.suffix in SKIP_SUFFIXES:
            continue
        if any(marker in lower_name for marker in SENSITIVE_NAME_MARKERS):
            print(f"  WARNING: skipping sensitive-named file: {rel.as_posix()}",
                  file=sys.stderr)
            continue
        if rel.as_posix() in out:
            continue
        out[rel.as_posix()] = full.read_bytes()
    return sorted(out.items())


def file_mode(path: str, blob: bytes) -> str:
    if path.endswith(".sh") or blob.startswith(b"#!"):
        return "100755"
    return "100644"


def publish(owner: str, repo: str, token: str, files) -> str:
    base = f"/repos/{owner}/{repo}"
    # Does the repo already have a commit on BRANCH?
    head_sha = None
    base_tree = None
    try:
        ref, _ = request("GET", f"{base}/git/ref/heads/{BRANCH}", token)
        head_sha = ref["object"]["sha"]
        commit, _ = request("GET", f"{base}/git/commits/{head_sha}", token)
        base_tree = commit["tree"]["sha"]
    except GitHubError as exc:
        # A brand-new, uninitialised repository answers 409 "Git Repository is
        # empty" (not 404) when asked for a ref.  Both mean "no base commit".
        if exc.status not in (404, 409):
            raise

    if head_sha is None:
        # The Git Data API refuses *all* writes (even POST /git/blobs) while a
        # repository has zero commits.  Seed the first commit through the
        # Contents API, which does work on an empty repository, then continue
        # normally with a single clean commit on top of it.
        seed = (b"# star 11u low-voltage kernel\n\n"
                b"Repository bootstrap; the full build project lands in the next commit.\n")
        request("PUT", f"{base}/contents/README.md", token, {
            "message": "Initialise repository",
            "content": base64.b64encode(seed).decode("ascii"),
            "branch": BRANCH,
        })
        ref, _ = request("GET", f"{base}/git/ref/heads/{BRANCH}", token)
        head_sha = ref["object"]["sha"]
        commit, _ = request("GET", f"{base}/git/commits/{head_sha}", token)
        base_tree = commit["tree"]["sha"]
        print("  seeded the empty repository with an initial commit", flush=True)

    print(f"  base commit: {head_sha}", flush=True)

    tree_entries = []
    for i, (path, blob) in enumerate(files, 1):
        payload = {
            "content": base64.b64encode(blob).decode("ascii"),
            "encoding": "base64",
        }
        created, _ = request("POST", f"{base}/git/blobs", token, payload)
        tree_entries.append({
            "path": path,
            "mode": file_mode(path, blob),
            "type": "blob",
            "sha": created["sha"],
        })
        print(f"  [{i}/{len(files)}] {path}", flush=True)

    tree_body = {"tree": tree_entries}
    if base_tree:
        tree_body["base_tree"] = base_tree
    tree, _ = request("POST", f"{base}/git/trees", token, tree_body)

    commit_body = {
        "message": "star/11u low-voltage kernel: voltage EMA filtering + shutdown debounce",
        "tree": tree["sha"],
    }
    if head_sha:
        commit_body["parents"] = [head_sha]
    commit, _ = request("POST", f"{base}/git/commits", token, commit_body)

    if head_sha:
        request("PATCH", f"{base}/git/refs/heads/{BRANCH}", token,
                {"sha": commit["sha"], "force": False})
    else:
        request("POST", f"{base}/git/refs", token,
                {"ref": f"refs/heads/{BRANCH}", "sha": commit["sha"]})
    request("PATCH", base, token, {"default_branch": BRANCH})
    return commit["sha"]


def latest_run(owner: str, repo: str, token: str):
    # Most recent run of any trigger (push or workflow_dispatch).  The push
    # trigger already fires the build with workflow defaults, so we rely on it
    # and just surface whatever run is newest.
    data, _ = request("GET", f"/repos/{owner}/{repo}/actions/runs?per_page=10", token)
    runs = data.get("workflow_runs", []) if data else []
    return runs[0] if runs else None


def dispatch(owner: str, repo: str, token: str, inputs: dict) -> dict | None:
    base = f"/repos/{owner}/{repo}"
    # Wait until GitHub has indexed the workflow file (new repos need a moment).
    for attempt in range(12):
        try:
            request("POST", f"{base}/actions/workflows/{WORKFLOW}/dispatches", token,
                    {"ref": BRANCH, "inputs": inputs})
            break
        except GitHubError as exc:
            if exc.status != 404 or attempt == 11:
                raise
            time.sleep(5)
    else:
        return None

    for _ in range(20):
        data, _ = request("GET", f"{base}/actions/runs?event=workflow_dispatch&per_page=5", token)
        runs = data.get("workflow_runs", []) if data else []
        if runs:
            return runs[0]
        time.sleep(3)
    # Fall back to the push-triggered run from the initial commit, if any.
    return latest_run(owner, repo, token)


def show_status(owner: str, repo: str, token: str, run: dict):
    print(f"\n  run #{run['run_number']}  status={run['status']}  "
          f"conclusion={run.get('conclusion') or '-'}")
    print(f"  {run['html_url']}")
    if run["status"] != "completed":
        return
    jobs, _ = request("GET", f"/repos/{owner}/{repo}/actions/runs/{run['id']}/jobs?per_page=20", token)
    for job in jobs.get("jobs", []):
        failed = [s["name"] for s in job.get("steps", []) if s.get("conclusion") == "failure"]
        print(f"  job {job['name']}: {job['conclusion']}"
              + (f"  failed steps: {', '.join(failed)}" if failed else ""))
    if run.get("conclusion") == "success":
        arts, _ = request("GET", f"/repos/{owner}/{repo}/actions/runs/{run['id']}/artifacts", token)
        for art in arts.get("artifacts", []):
            print(f"  artifact: {art['name']}  ({art['size_in_bytes'] / 1048576:.1f} MiB)")
        print(f"  download: https://github.com/{owner}/{repo}/actions/runs/{run['id']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--token", default=None)
    ap.add_argument("--token-file", default=None,
                    help="read the PAT from this file (first line, trimmed)")
    ap.add_argument("--repo", default="star-11u-lowvolt-kernel")
    ap.add_argument("--private", action="store_true", help="create a private repository")
    ap.add_argument("--follow", action="store_true", help="poll until the build finishes")
    ap.add_argument("--status", action="store_true", help="only report the latest run")
    ap.add_argument("--dispatch", action="store_true",
                    help="also fire an explicit workflow_dispatch (e.g. to pass custom inputs); "
                         "by default the push event already triggers the build with defaults")
    ap.add_argument("--shutdown-mv", default=DEFAULT_INPUTS["shutdown_mv"])
    ap.add_argument("--table", default=DEFAULT_INPUTS["volt_soc_table"])
    ap.add_argument("--upstream-ref", default=DEFAULT_INPUTS["upstream_ref"])
    ap.add_argument("--llvm-ias", default=DEFAULT_INPUTS["llvm_ias"], choices=["0", "1"])
    args = ap.parse_args()

    token = (args.token or os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN"))
    if not token:
        candidates = ([Path(args.token_file)] if args.token_file else []) + [
            ROOT / ".gh_token", Path.home() / ".gh_token",
        ]
        for candidate in candidates:
            try:
                token = candidate.read_text(encoding="utf-8").strip().splitlines()[0].strip()
                if token:
                    print(f"  using token from {candidate}")
                    break
            except (OSError, IndexError):
                continue
    if not token:
        sys.exit("ERROR: provide --token, GH_TOKEN, --token-file, or a .gh_token file "
                 "(classic PAT with 'repo' + 'workflow').")
    args.token = token

    print("[1/5] Verifying token ...", flush=True)
    user, headers = request("GET", "/user", args.token)
    owner = user["login"]
    scopes = headers.get("X-OAuth-Scopes", "")
    print(f"  authenticated as {owner}  scopes: {scopes or '(fine-grained / none reported)'}")
    if scopes and "workflow" not in scopes:
        print("  WARNING: token lacks the 'workflow' scope; publishing the workflow file may fail.")

    base = f"/repos/{owner}/{args.repo}"
    if args.status:
        run = latest_run(owner, args.repo, args.token)
        if not run:
            sys.exit("No Actions run found yet.")
        show_status(owner, args.repo, args.token, run)
        return 0

    files = collect_files()
    # Hard safety net: refuse to upload anything containing the live PAT.
    pat = args.token.encode("utf-8")
    for path, blob in files:
        if pat in blob:
            sys.exit(f"ERROR: refusing to publish {path}: it contains the live PAT")
    print(f"[2/5] {len(files)} files to publish", flush=True)

    print("[3/5] Creating/using repository ...", flush=True)
    try:
        created, _ = request("POST", "/user/repos", args.token, {
            "name": args.repo,
            "description": "Xiaomi Mi 11 Ultra (star) kernel: 2980 mV cutoff + rebuilt voltage-SOC table",
            "private": args.private,
            "auto_init": False,
            "has_issues": False,
            "has_projects": False,
            "has_wiki": False,
        })
        print(f"  created {created['html_url']}")
    except GitHubError as exc:
        if exc.status not in (422, 403):
            raise
        info, _ = request("GET", base, args.token)
        print(f"  reusing existing {info['html_url']}")

    commit_sha = publish(owner, args.repo, args.token, files)
    print(f"  commit {commit_sha[:12]} pushed to {BRANCH}")
    repo_url = f"https://github.com/{owner}/{args.repo}"
    print(f"  repo: {repo_url}")

    if args.dispatch:
        inputs = dict(DEFAULT_INPUTS)
        inputs.update({
            "upstream_ref": args.upstream_ref,
            "shutdown_mv": args.shutdown_mv,
            "volt_soc_table": args.table,
            "llvm_ias": args.llvm_ias,
        })
        print("[4/5] Triggering explicit workflow_dispatch ...", flush=True)
        run = dispatch(owner, args.repo, args.token, inputs)
        if not run:
            print("  dispatch accepted but no run appeared yet; check:", flush=True)
            print(f"  {repo_url}/actions")
            return 0
        print(f"  run #{run['run_number']}: {run['html_url']}")
    else:
        # The push event above already triggers the build (paths: overlay/,
        # patches/, config/, .github/workflows/).  Wait for that run to appear.
        print("[4/5] push event triggers the build; waiting for the run ...", flush=True)
        run = None
        for _ in range(20):
            run = latest_run(owner, args.repo, args.token)
            if run and run["status"] != "completed":
                break
            time.sleep(5)
        if not run:
            print("  no run appeared yet; check:", flush=True)
            print(f"  {repo_url}/actions")
            return 0
        print(f"  run #{run['run_number']}: {run['html_url']}")

    print("[5/5] Build status", flush=True)
    show_status(owner, args.repo, args.token, run)
    if args.follow:
        while run and run["status"] != "completed":
            time.sleep(30)
            fresh, _ = request("GET", f"{base}/actions/runs/{run['id']}", args.token)
            run = fresh
            print(f"  ... status={run['status']}", flush=True)
        show_status(owner, args.repo, args.token, run)
    print("\nDone. Artifacts: Image.gz + AnyKernel3 zip (in the run's Artifacts section).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (GitHubError, RuntimeError) as exc:
        sys.exit(f"ERROR: {exc}")
