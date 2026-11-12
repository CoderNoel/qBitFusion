# Contributing to qBitFusion

Thank you for considering it. This project is maintained in spare time, so contributions that are small,
tested and easy to review are the ones that get merged.

By taking part you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## Where help is most useful

- **More debrid providers.** Real-Debrid is the only adapter. A new one implements
  [`Hybrid::Provider`](src/base/hybrid/provider.h) and comes with contract tests; it must not change the
  coordinator or the state machine.
- **Linux and macOS.** Nobody has built or tested the hybrid code there yet. A secure credential store is
  needed on both (Secret Service / Keychain).
- **Web UI and Web API** controls for the download source.
- **A packet-level check** that "Debrid only" really produces no swarm traffic.
- **Translations** of the new text.

Current gaps and unverified claims are listed in [qbitfusion/docs/STATUS.md](qbitfusion/docs/STATUS.md).

## Reporting a bug

Use the bug report form. Please check first whether the problem also happens in stock qBittorrent 5.3:

- If it does, it is an upstream bug. Report it to
  [qBittorrent](https://github.com/qbittorrent/qBittorrent/issues), following their rules.
- If it only happens in qBitFusion, report it here. Never report qBitFusion problems upstream.

Before attaching a log or a screenshot, remove anything private: provider account names, tokens,
download links, IP addresses and the names of what you download. qBitFusion keeps tokens and signed links
out of its log, but torrent names and addresses are in it.

Security problems are reported privately. See [SECURITY.md](SECURITY.md).

## Building and testing

The Windows scripts in `qbitfusion/scripts` reproduce the toolchain and run everything:

```powershell
pwsh qbitfusion/scripts/bootstrap-windows.ps1   # once
pwsh qbitfusion/scripts/build-windows.ps1       # build + all test suites
```

To look at a change in the running app without touching your own profile:

```powershell
$id = pwsh qbitfusion/scripts/test-instance.ps1
pwsh qbitfusion/scripts/capture-windows.ps1 -ProcessId $id -OutDir build/ui-smoke -Prefix check
```

## Rules for a change

The design rules are at the top of [qbitfusion/docs/SPECIFICATION.md](qbitfusion/docs/SPECIFICATION.md#design-rules). They are not
optional. The short version:

1. libtorrent owns storage, hashing and resume data. Provider bytes never go straight to a torrent's files.
2. Only hash-verified pieces count as progress.
3. "Debrid only" produces no swarm traffic. BitTorrent fallback needs the user's explicit consent.
4. No tokens or signed links in code, logs, screenshots, tests or commits.
5. Nothing that works around a provider's limits, blocks or takedowns.
6. With the feature off, behavior is the same as upstream.
7. A claim needs proof: a test, a measurement or a repeatable manual check.

And for the code itself:

- Follow [CODING_GUIDELINES.md](CODING_GUIDELINES.md) (inherited from qBittorrent).
- Keep changes to upstream files small and marked with a `qBitFusion:` comment, so that merging upstream
  releases stays possible. Prefer new files in `src/base/hybrid`, `src/gui/hybrid` and `src/gui/modern`.
  See [qbitfusion/docs/UPSTREAM.md](qbitfusion/docs/UPSTREAM.md).
- Changes to the look go behind `ModernAppearance::isActive()`.
- Add tests for new behavior and for its failure paths. A pull request must build without warnings
  (`/WX`) and pass every suite.
- UI changes include a before/after screenshot.
- Releases and the updater are described in [qbitfusion/docs/RELEASING.md](qbitfusion/docs/RELEASING.md).
- Every pull request is also checked by the repository's pre-commit hooks (formatting, spelling, Markdown
  lint, `.ui` grid order). To run them yourself: `pip install prek`, then `prek run --all-files`.

## Pull requests

- One topic per pull request. Describe what changed, why, and how you tested it.
- Say plainly what you did not test.
- AI-assisted work is accepted here, on the condition that a person has reviewed it, understands it and
  takes responsibility for it. (Upstream qBittorrent has a stricter policy; follow theirs when
  contributing there.)
- By submitting a contribution you agree that it is licensed under the project's license, GPL-2.0-or-later
  with the OpenSSL exception (see [COPYING](COPYING)).
