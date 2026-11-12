# Following upstream qBittorrent

qBitFusion is a fork of [qBittorrent](https://github.com/qbittorrent/qBittorrent). This page explains how
the two relate and how to bring upstream fixes into this repository.

## Where the fork starts

- Base: the upstream tag `release-5.3.0` (commit `9213075dd`).
- Everything after that tag on the default branch is qBitFusion's own work.
- The upstream history is kept on purpose. It is what makes `git merge` with a later upstream release
  possible, it preserves the authorship of every upstream contributor, and it is the normal way to publish
  a fork of a GPL project.

## What happens automatically

The workflow [`upstream-sync.yaml`](../../.github/workflows/upstream-sync.yaml) runs once a day.

- **A newer qBittorrent release in the same series** (for example 5.3.1 while on 5.3.0) is merged on a
  branch named `upstream/release-5.3.1`, and a pull request is opened.
  - If it merged without conflicts, the Windows build and all tests run on it, and the pull request merges
    itself when they pass. On its next run the workflow starts the [Release](RELEASING.md) workflow, and
    installed copies update themselves. Nobody has to do anything.
  - If it has conflicts, the pull request stays open for a person, or an agent working for one, to
    resolve. The pull request's description says how.
- **A new series** (5.4, 6.0) is always a pull request for review, even when it merges cleanly. A new series
  can raise the required Qt, libtorrent and compiler versions, and the hooks in the session and torrent
  classes have to be checked against the new code.

What this cannot catch: a clean merge with passing tests can still break something the tests do not cover.
The hybrid transport's own tests run on every merge, but nothing here downloads a real torrent from a real
provider. Watch the log of the first release after an upstream merge.

To stop automatic merging, delete the `gh pr merge ... --auto` line at the end of the workflow; every
upstream release then waits as a pull request.

## Merging by hand

For a conflicting pull request, or to merge an upstream release before the daily run:

```bash
git remote add upstream https://github.com/qbittorrent/qBittorrent.git   # once
git fetch upstream --tags
git switch -c merge/release-5.3.1 origin/main
git merge release-5.3.1
```

Resolve conflicts, then build, run every test and check the feature-off behavior before opening a pull
request:

```powershell
pwsh qbitfusion/scripts/build-windows.ps1
```

Merge the pull request with a **merge commit**, never squash or rebase: the merge commit is what tells git
that the upstream release is included, so that the next one merges cleanly.

## Where conflicts are likely

qBitFusion adds most of its code in new directories, which never conflict:

- `src/base/hybrid/`, `src/gui/hybrid/` — the hybrid transport and its GUI
- `src/gui/modern/` — the modern look, icons and shell widgets
- `qbitfusion/` — documentation, scripts, the spike and the branding check
- `test/testhybrid*.cpp`

It also edits about 60 upstream files. The ones that carry the largest changes, and so the highest chance
of a conflict:

| File | Why it was changed | How to resolve a conflict |
| --- | --- | --- |
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
