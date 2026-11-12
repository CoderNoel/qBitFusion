# Releases and updates

How a qBitFusion release is made, and how installed copies update themselves.

## Versions

A qBitFusion version is the qBittorrent version it is based on, plus a revision:

- `5.3.0.1` is the first qBitFusion release on qBittorrent 5.3.0, `5.3.0.2` the next one.
- After qBittorrent 5.3.1 is merged, the next release is `5.3.1.1`.
- Revision 0 is a development build: anything built locally, including with
  `qbitfusion/scripts/install-windows.ps1` and `package-windows.ps1`. A development build never updates
  itself, and About shows it as "development build".

Each release is tagged `v<version>`, for example `v5.3.0.1`. Do not create tags starting with `release-`:
those are qBittorrent's.

## Making a release

Releases are built on GitHub, never on a personal machine, so that every release is built the same way and
signed with the release key.

- **This project's own changes:** Actions > Release > Run workflow (on `main`). It builds with the next
  revision, runs every test, packages, signs and publishes. If the current commit is already released, it
  does nothing.
- **qBittorrent releases:** nothing to do. [upstream-sync](UPSTREAM.md#what-happens-automatically) merges
  them and starts the Release workflow itself.

Each release carries four files:

| File | Purpose |
| --- | --- |
| `qBitFusion-<version>-windows-x64.zip` | The portable package. |
| `qBitFusion-<version>-windows-x64.zip.sha256` | Its checksum, for people who download it by hand. |
| `update.json` | The version, and the package's URL, size and SHA-256. |
| `update.json.sig` | An Ed25519 signature over `update.json`, made with the release key. |

## How installed copies update

1. Once a day (and from the menu, Help > Check for Updates) the program fetches `update.json` and
   `update.json.sig` from the latest release.
2. It checks the signature against the public key built into it (`UPDATE_PUBLIC_KEY` in
   `src/base/selfupdate/selfupdater.cpp`). Anything not signed with the release key is ignored.
3. If the version is newer, it downloads the package in the background, checks its size and SHA-256
   against the signed `update.json`, and unpacks it into `update/staged` next to the program.
4. The next time the program starts, it starts the new version from `update/staged` with
   `--qbf-apply-update`, and exits. The new version waits for the old one to close, swaps the files in
   (keeping the `profile` folder, and the replaced files in `update/previous`), and starts the program
   again. If any file cannot be replaced, everything is put back.
5. The program reports the result in its log and with a notification.

Updates are off when Options > Behavior > "Keep qBitFusion up to date" is unchecked, for development
builds, and when the program's folder is not writable (for example under Program Files).

## The release key

The private key signs `update.json`. It is stored as the secret `QBF_UPDATE_SIGNING_KEY` of the GitHub
environment `release`, which only workflow runs on `main` can use. The Release workflow refuses to run if
the secret is missing or does not match the public key in the source.

**Keep a copy of the private key outside GitHub,** for example in a password manager. GitHub secrets cannot
be read back. If the key is lost, installed copies cannot be updated anymore: a release signed with a new
key would be refused by every copy that has the old public key built in, and their users would have to
download the new version by hand once.

If the key is ever exposed, generate a new one, put its public half in `selfupdater.cpp`, release, and tell
users to install that release by hand.

To create a key (OpenSSL 3):

```bash
openssl genpkey -algorithm ED25519 -out update-signing-key.pem
openssl pkey -in update-signing-key.pem -pubout -outform DER | tail -c 32 | base64   # the public key
```
