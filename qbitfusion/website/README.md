# Website

The project's website, published at <https://codernoel.github.io/qBitFusion/> by the
[Website workflow](../../.github/workflows/website.yaml) whenever these files change on `main`.

It is plain HTML, CSS and JavaScript with no build tools or dependencies. It loads no third-party fonts,
analytics or cookies; the only outside request is to the GitHub API, for the latest release.

| File | Contents |
| --- | --- |
| `index.html` | The page. |
| `styles.css` | All styling. The accent color is the `--accent` variable. |
| `script.js` | Scroll effects, the source illustration, the accent picker and the release lookup. |
| `assets/` | Logo, favicon and the social preview image (`og.png`, 1200 × 630). |

The screenshots are not kept here. They are copied from `qbitfusion/docs/screenshots/` when the site is
built, so the README and the site always show the same images.

## Preview

```sh
python qbitfusion/scripts/build-website.py
python -m http.server -d build-website 8000
```

Then open <http://localhost:8000>.

## Downloads

The download button looks up the newest non-draft release through the GitHub API and links to its
`*-windows-x64.zip` asset (and the `.sha256` beside it). If there is no such release, or the request
fails, the page shows the build-from-source steps instead. Nothing about releases is configured here.

## Keeping it accurate

The text repeats facts from the main [README](../../README.md): test counts, limitations and open work.
When those change, update the Status and Contribute sections too.
