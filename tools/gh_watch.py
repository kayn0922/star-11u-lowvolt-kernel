#!/usr/bin/env python3
"""Watch a GitHub Actions run until it finishes and report the result.

Usage:
  python tools/gh_watch.py --run-id 43455189468
  python tools/gh_watch.py --run-id 43455189468 --interval 60 --max-minutes 60

Reads the PAT from .gh_token (or GH_TOKEN).  On completion it prints the
conclusion, failed steps, the tail of the job log and the artifact list.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"
ROOT = pathlib.Path(__file__).resolve().parent.parent
OWNER = "kayn0922"
REPO_NAME = "star-11u-lowvolt-kernel"
REPO = f"/repos/{OWNER}/{REPO_NAME}"


def headers(token: str) -> dict:
    return {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "star-11u-lowvolt-watcher",
    }


def get_json(path: str, token: str):
    req = urllib.request.Request(API + path, headers=headers(token))
    with urllib.request.urlopen(req, timeout=90) as resp:
        raw = resp.read()
        return json.loads(raw) if raw else None


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Keep the Authorization header off the signed log-download redirect."""

    def redirect_request(self, req, fp, code, msg, hdrs, newurl):
        return None


def fetch_job_log(job_id: int, token: str) -> str:
    url = f"{API}{REPO}/actions/jobs/{job_id}/logs"
    req = urllib.request.Request(url, headers=headers(token))
    opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(req, timeout=90) as resp:
            return resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        if exc.code not in (301, 302, 303, 307, 308):
            raise
        location = exc.headers.get("Location")
        if not location:
            raise
        # Signed URL: must be fetched WITHOUT the GitHub Authorization header.
        with urllib.request.urlopen(location, timeout=180) as resp:
            return resp.read().decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-id", type=int, required=True)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--max-minutes", type=int, default=60)
    ap.add_argument("--tail", type=int, default=80)
    args = ap.parse_args()

    token = os.environ.get("GH_TOKEN")
    if not token:
        for candidate in (ROOT / ".gh_token", pathlib.Path.home() / ".gh_token"):
            try:
                token = candidate.read_text(encoding="utf-8").strip().splitlines()[0].strip()
                if token:
                    break
            except (OSError, IndexError):
                continue
    if not token:
        sys.exit("ERROR: no PAT available (.gh_token / GH_TOKEN)")

    deadline = time.time() + args.max_minutes * 60
    last_step = ""
    while time.time() < deadline:
        try:
            run = get_json(f"{REPO}/actions/runs/{args.run_id}", token)
        except Exception as exc:
            print(f"[{time.strftime('%H:%M:%S')}] poll error: {exc}", flush=True)
            time.sleep(args.interval)
            continue

        if run["status"] == "completed":
            conclusion = run.get("conclusion")
            print(f"=== FINISHED: {conclusion} ===", flush=True)
            print(run["html_url"], flush=True)
            jobs = get_json(f"{REPO}/actions/runs/{args.run_id}/jobs?per_page=10", token)
            for job in jobs.get("jobs", []):
                print(f"JOB {job['name']}: {job['conclusion']}", flush=True)
                for step in job.get("steps", []):
                    if step.get("conclusion") not in (None, "success", "skipped"):
                        print(f"  FAILED STEP {step['number']}: {step['name']} "
                              f"({step.get('conclusion')})", flush=True)
                try:
                    text = fetch_job_log(job["id"], token)
                    lines = [l for l in text.splitlines() if l.strip()]
                    print(f"  --- log tail ({len(lines)} lines total) ---", flush=True)
                    for line in lines[-args.tail:]:
                        print("   ", line[:400], flush=True)
                except Exception as exc:
                    print(f"  (job log unavailable: {exc})", flush=True)
            try:
                arts = get_json(f"{REPO}/actions/runs/{args.run_id}/artifacts", token)
                for art in arts.get("artifacts", []):
                    print(f"ARTIFACT {art['name']}  {art['size_in_bytes']} bytes  "
                          f"id={art['id']}", flush=True)
            except Exception as exc:
                print(f"  (artifacts unavailable: {exc})", flush=True)
            return 0 if conclusion == "success" else 1

        try:
            jobs = get_json(f"{REPO}/actions/runs/{args.run_id}/jobs?per_page=5", token)
            cur = ""
            for job in jobs.get("jobs", []):
                cur = next((s["name"] for s in job.get("steps", [])
                            if s["status"] == "in_progress"), "")
            if cur != last_step:
                print(f"[{time.strftime('%H:%M:%S')}] step: {cur or '(queued)'}", flush=True)
                last_step = cur
            else:
                print(f"[{time.strftime('%H:%M:%S')}] still: {cur or '(queued)'}", flush=True)
        except Exception:
            pass
        time.sleep(args.interval)

    print(f"WATCH TIMEOUT after {args.max_minutes} minutes; run still active.", flush=True)
    return 2


if __name__ == "__main__":
    sys.exit(main())
