# Following upstream qBittorrent

qBitFusion is a fork of [qBittorrent](https://github.com/qbittorrent/qBittorrent). This page explains how
the two relate and how to bring upstream fixes into this repository.

## Where the fork starts

- Base: the upstream tag `release-5.3.0` (commit `9213075dd`).
- Everything after that tag on the default branch is qBitFusion's own work.
- The upstream history is kept on purpose. It is what makes `git merge` with a later upstream release
  possible, it preserves the authorship of every upstream contributor, and it is the normal way to publish
  a fork of a GPL project.

## What can and cannot be automatic

Upstream fixes do not arrive here by themselves. A person merges them, because qBitFusion changes some of
the same files and a merge can conflict.

What is automated: the workflow [`upstream-watch.yaml`](../.github/workflows/upstream-watch.yaml) checks
once a week whether upstream has published a newer release in the series this fork is based on, and opens
an issue when it has. (It has not run yet at the time of writing; treat it as untested until its first
run.)

## Merging an upstream release

Point releases of the same series (for example `release-5.3.1`) are the easy and recommended case.

```bash
git remote add upstream https://github.com/qbittorrent/qBittorrent.git   # once
git fetch upstream --tags
git switch -c merge/release-5.3.1
git merge release-5.3.1
```

Resolve conflicts, then build, run every test and check the feature-off behaviour before opening a pull
request:

```powershell
pwsh qbitfusion/scripts/build-windows.ps1
```

A new minor or major series (5.4, 6.0) is a porting job, not a routine merge: the hooks in the session and
torrent classes have to be re-checked against the new code, and the libtorrent and Qt versions may move.

## Where conflicts are likely

qBitFusion adds most of its code in new directories, which never conflict:

- `src/base/hybrid/`, `src/gui/hybrid/` — the hybrid transport and its GUI
- `src/gui/modern/` — the modern look, icons and shell widgets
- `qbitfusion/`, `docs/` — scripts, notes and documentation
- `test/testhybrid*.cpp`

It also edits about 60 upstream files. The ones that carry the largest changes, and so the highest chance
of a conflict:

| File | Why it was changed | How to resolve a conflict |
|---|---|---|
| `src/gui/addnewtorrentdialog.ui`, `.cpp` | The dialog layout was rebuilt (same object names). | Keep this fork's layout, then add any widget upstream introduced by hand. |
| `src/gui/mainwindow.cpp` | Header and action bar replace the menu bar and toolbar. | Keep both sides; new upstream menu actions appear in the overflow menu automatically. |
| `src/base/bittorrent/sessionimpl.cpp`, `torrentimpl.cpp` | Small hooks: hold at add/load, piece alerts, recheck, HTTPS rate. | Re-apply the hooks around upstream's new code. They are marked with `qBitFusion:` comments. |
| `src/gui/transferlistwidget.cpp`, `transferlistmodel.cpp` | Source column and its menu. | Keep both sides. |
| `src/gui/optionsdialog.cpp`, `.ui` | Hybrid page, accent picker, page list. | Keep both sides. |
| Text that says "qBittorrent" in the UI | Renamed to qBitFusion. | Keep the new name in user-visible text. |
| `.github/`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md` | Replaced for this project. | Keep this fork's version. |
| `AGENTS.md` | Removed: it states upstream's policy for upstream's repository. | Keep it deleted. |

To see every upstream file this fork has touched:

```bash
git diff --name-status release-5.3.0 HEAD | grep '^M'
```

## Sending changes the other way

A fix for something that is also wrong in stock qBittorrent belongs upstream, and has to follow upstream's
own contribution rules. Note that qBittorrent strongly discourages issues and pull requests written or
submitted by AI agents (see their
[AGENTS.md](https://github.com/qbittorrent/qBittorrent/blob/master/AGENTS.md)): a person must review, take
responsibility for and submit such a change.

Never open qBitFusion-specific issues or pull requests in the upstream repository.
