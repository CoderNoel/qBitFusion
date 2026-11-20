"""Assemble the project website into a folder that can be served as-is.

The site's own files live in qbitfusion/website/. The screenshots are not copied there; they are taken
from qbitfusion/docs/screenshots/ at build time, so the README and the site always show the same images.

With --with-release, the zip of the latest GitHub release is downloaded, checked against the size and
SHA-256 in that release's update.json, and placed in download/ with a latest.json describing it. The
site then offers the download from its own domain, so visitors stay on the page.

    python qbitfusion/scripts/build-website.py                               # writes build-website/
    python qbitfusion/scripts/build-website.py --with-release --out _site    # what the Pages workflow does
    python -m http.server -d build-website 8000                              # preview at http://localhost:8000

GITHUB_REPOSITORY selects the repository (default CoderNoel/qBitFusion). GITHUB_TOKEN, if set, is sent to
the GitHub API to avoid its rate limit for anonymous requests; it is never written anywhere.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import urllib.error
import urllib.request
from http.client import HTTPResponse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "qbitfusion" / "website"
SCREENSHOTS = ROOT / "qbitfusion" / "docs" / "screenshots"
ZIP_NAME = re.compile(r"^qBitFusion-[0-9.]+-windows-x64\.zip$")


def fetch(url: str, *, api: bool = False) -> HTTPResponse:
    headers = {"User-Agent": "qbitfusion-website-build"}
    if api:
        headers["Accept"] = "application/vnd.github+json"
        token = os.environ.get("GITHUB_TOKEN")
        if token:
            headers["Authorization"] = f"Bearer {token}"
    response: HTTPResponse = urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60)
    return response


def add_release(out: Path) -> None:
    repo = os.environ.get("GITHUB_REPOSITORY", "CoderNoel/qBitFusion")
    try:
        with fetch(f"https://api.github.com/repos/{repo}/releases/latest", api=True) as response:
            release = json.load(response)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        print("There is no release yet; the page will show the build-from-source steps.")
        return
    assets = {asset["name"]: asset for asset in release["assets"]}
    if "update.json" not in assets:
        raise SystemExit(f"The latest release, {release['tag_name']}, has no update.json.")
    with fetch(assets["update.json"]["browser_download_url"]) as response:
        update = json.load(response)

    entry = update["windows-x64"]
    name = entry["url"].rsplit("/", 1)[-1]
    if not ZIP_NAME.match(name) or name not in assets:
        raise SystemExit(f"update.json names an unexpected file: {name}")

    folder = out / "download"
    folder.mkdir()
    target = folder / name
    digest = hashlib.sha256()
    with fetch(assets[name]["browser_download_url"]) as response, target.open("wb") as file:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            file.write(chunk)
    size = target.stat().st_size
    if size != entry["size"] or digest.hexdigest() != entry["sha256"].lower():
        raise SystemExit(f"{name} does not match the size and SHA-256 in update.json.")

    (folder / f"{name}.sha256").write_text(f"{entry['sha256'].lower()}  {name}\n", encoding="utf-8", newline="\n")
    latest = {
        "version": update["version"],
        "file": name,
        "size": size,
        "sha256": entry["sha256"].lower(),
        "published": release["published_at"],
        "notes": release["html_url"],
    }
    (folder / "latest.json").write_text(json.dumps(latest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Added {name} ({size} bytes) from {release['tag_name']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "build-website", help="output folder (replaced)")
    parser.add_argument("--with-release", action="store_true", help="include the latest release's zip")
    args = parser.parse_args()
    out = args.out.resolve()

    if out == ROOT or ROOT not in out.parents:
        raise SystemExit(f"Refusing to write outside the repository: {out}")

    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SITE, out, ignore=shutil.ignore_patterns("README.md"))
    shutil.copytree(SCREENSHOTS, out / "assets" / "screenshots")
    if args.with_release:
        add_release(out)
    print(f"Website written to {out}")


if __name__ == "__main__":
    main()
