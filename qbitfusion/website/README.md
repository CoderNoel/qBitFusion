# Website

The project's website, published at <https://qbitfusion.org/> by the
[Website workflow](../../.github/workflows/website.yaml) whenever these files change on `main`.

It is plain HTML, CSS and JavaScript with no build tools or dependencies. It loads no third-party fonts,
analytics or cookies, and makes no requests to other sites.

| File | Contents |
| --- | --- |
| `index.html` | The page. |
| `styles.css` | All styling. The accent color is the `--accent` variable. |
| `script.js` | Scroll effects, the illustrations, the accent picker (which also recolors the screenshots) and the download button. |
| `assets/` | Logo, favicon and the social preview image (`og.png`, 1200 × 630). |

The screenshots are not kept here. They are copied from `qbitfusion/docs/screenshots/` when the site is
built, so the README and the site always show the same images.

## Preview

```sh
python qbitfusion/scripts/build-website.py --with-release
python -m http.server -d build-website 8000
```

Then open <http://localhost:8000>. Leave out `--with-release` to skip downloading the release zip; the
page then shows the build-from-source steps where the download button would be.

## Domain

`qbitfusion.org` is registered at Namecheap and set as the custom domain in the repository's Pages
settings (it is also a verified domain on the owner's GitHub account). Its DNS has the four GitHub Pages
`A` and `AAAA` records for `@`, and `www` is a `CNAME` to `codernoel.github.io`. The old address,
`codernoel.github.io/qBitFusion/`, redirects to it.

## Downloads

The zip is served from the site itself, so a visitor stays on the page while it downloads. When the
Website workflow builds the site, `build-website.py --with-release` takes the latest GitHub release,
downloads its `*-windows-x64.zip`, checks it against the size and SHA-256 in that release's
`update.json`, and writes it to `download/` with a `.sha256` file and `latest.json`. The page reads
`latest.json` to label the button. Without it, the page shows the build-from-source steps instead.

The download follows the latest release automatically:

- the Release workflow starts the Website workflow on `main` after publishing;
- a release published, edited or deleted by hand on GitHub starts it too;
- every six hours the Website workflow compares the SHA-256 in the site's `latest.json` with the latest
  release's `update.json`, and rebuilds the site if they differ.

If there is no release, the site shows the build-from-source steps. Installed copies are not involved:
they update from GitHub Releases through `update.json`.

GitHub Pages has a soft bandwidth limit of 100 GB a month, roughly 3,000 downloads of a 32 MB zip. If
downloads approach that, link the button to the release asset again or move the file to other hosting.

## Keeping it accurate

The text repeats facts from the main [README](../../README.md): test counts, limitations and open work.
When those change, update the Status and Contribute sections too.
