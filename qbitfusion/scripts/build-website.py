#!/usr/bin/env python3
"""Assemble the project website into a folder that can be served as-is.

The site's own files live in qbitfusion/website/. The screenshots are not copied there; they are taken
from qbitfusion/docs/screenshots/ at build time, so the README and the site always show the same images.

    python qbitfusion/scripts/build-website.py              # writes build-website/
    python qbitfusion/scripts/build-website.py --out _site  # what the Pages workflow does
    python -m http.server -d build-website 8000             # preview at http://localhost:8000
"""

import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "qbitfusion" / "website"
SCREENSHOTS = ROOT / "qbitfusion" / "docs" / "screenshots"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "build-website", help="output folder (replaced)")
    out = parser.parse_args().out.resolve()

    if out == ROOT or ROOT not in out.parents:
        raise SystemExit(f"Refusing to write outside the repository: {out}")

    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SITE, out, ignore=shutil.ignore_patterns("README.md"))
    shutil.copytree(SCREENSHOTS, out / "assets" / "screenshots")
    print(f"Website written to {out}")


if __name__ == "__main__":
    main()
