# libtorrent feasibility spike

A stand-alone program that checks, against the pinned libtorrent build, the assumptions the hybrid transport
depends on: that `add_piece` verifies externally supplied pieces while a torrent is paused, that a held
torrent contacts no tracker or peer, that verified pieces survive a restart, and that the hand-over between
HTTPS and peers loses nothing that was verified.

It links only libtorrent, uses nothing outside `127.0.0.1`, and generates its data deterministically.

```powershell
pwsh qbitfusion/scripts/build-windows.ps1 -Spike
```

Every check prints `PASS`, `FAIL` or `INFO`, and the program exits non-zero on any failure. The results the
design is based on are recorded in [docs/DECISIONS.md](../../docs/DECISIONS.md).
