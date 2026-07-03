# Save Keeper

Native PS Vita save backup and restore app with planned Google Drive snapshot support.

## Current status

This repository currently contains the first buildable foundation:

- host-tested core logic for JKSV-style timestamped backup names
- local/remote backup menu entries, with only remote entries shown as `[GD] ...`
- path component normalization for future local and Google Drive folder names
- startup discovery for Vita saves, game-card saves, and PSP/Adrenaline saves
- Vita title/icon metadata from `ur0:/shell/db/app.db`, following SaveCloud Vita's app database
  mapping approach
- `PARAM.SFO` title and `ICON0.PNG` fallback metadata for PSP-style saves
- local timestamped ZIP backup creation
- local backup listing for the selected save
- local backup restore with a second-press confirmation
- Google OAuth device flow with QR sign-in and automatic token polling
- Google Drive upload for the selected local ZIP backup
- Google Drive listing and download-then-restore for `[GD]` ZIP backups
- a native `vita2d` grid UI with game icons, QR auth display, and packaged LiveArea assets

Delete/rename flows are not implemented yet.

## Target behavior

- The app is a standalone Vita VPK for normal use.
- Google auth should use the OAuth device flow: the Vita displays a code and QR link, then a phone
  or PC browser completes the Google consent step.
- Backups are timestamped ZIP snapshots named like `2026-05-21 16-14.zip`.
- Multiple backup timestamps can exist for the same title.
- Local saves stay untagged in the UI.
- Google Drive entries are shown with a `[GD]` prefix, following the JKSV convention.
- The planned Drive layout is `PSV Saves/<title-or-save-id>/<timestamp>.zip`.

## Build and test

Host tests:

```sh
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Vita VPK build:

```sh
export VITASDK=/path/to/vitasdk
cmake -S . -B build/vita -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build/vita
```

The VPK is written to `build/vita/save-keeper.vpk`.

## Project layout

- `src/core`: portable logic covered by host tests
- `src/vita`: Vita app loop and native UI
- `tests`: lightweight host test executable
- `sce_sys`: Vita package metadata

LiveArea PNGs under `sce_sys` must remain 8-bit indexed PNGs. VitaShell can fail promotion with
`0x8010113D` when package images are regular RGBA PNGs.

## Save roots

The current foundation scans these roots at startup:

- `ux0:user/00/savedata`
- `grw0:savedata`
- `ux0:pspemu/PSP/SAVEDATA`

Vita titles are matched against `ur0:/shell/db/app.db` so saves with internal folder names can show
the installed game's title and system icon. If the app database lookup fails, Save Keeper falls back
to metadata inside the save folder where available.

Local backups are written under `ux0:data/save-keeper/backups`.

## Google auth

See [docs/google-drive-setup.md](docs/google-drive-setup.md) for the one-time Google Cloud setup
(OAuth client of type "TVs and Limited Input devices", published to production so refresh tokens
do not expire after 7 days). Credentials are either embedded at build time via the
`SAVE_KEEPER_GOOGLE_CLIENT_ID`/`SAVE_KEEPER_GOOGLE_CLIENT_SECRET` CMake options or read from
`ux0:data/save-keeper/google-client.json`.

Connecting takes a single Triangle press: the Vita shows a QR code (with the sign-in code
pre-filled for the phone) and polls Google automatically until access is approved. The token cache
is saved to `ux0:data/save-keeper/google-token.json`, is refreshed automatically, and survives app
exits and reboots.

After auth, press Triangle to refresh remote backups for the selected save. Press Select to upload
the selected local backup. Press Square on a local backup to restore it, or on a `[GD]` backup to
download it locally and then restore it.

HTTPS uses the Mozilla CA bundle packaged in the VPK (`sce_sys/resources/cacert.pem`); TLS
verification stays enabled.

## Vita controls

- D-Pad: move through the save grid
- L/R: move through backups for the selected save
- Circle: create a new timestamped local backup
- Square: restore selected local or `[GD]` backup, with a second press to confirm
- Select: upload selected local backup to Google Drive
- Triangle: connect Google Drive (one press, then approve on a phone) or refresh remote backups
- Cross: cancel restore confirmation or the Google sign-in

The app does not expose a START exit shortcut; use the Vita home button to leave the app.

## Third-party code

- `third_party/qrcodegen`: Project Nayuki QR Code generator, MIT License.
- `third_party/vitasqlite`: VitaSmith's SQLite R/W VFS override for Sony's SceSqlite module,
  GPLv3 (the same file VitaShell and Apollo Save Tool use to read `ur0:shell/db/app.db`).

## Comment style

Straightforward code should stay straightforward. Logic that encodes a product decision, external
API behavior, Vita-specific constraint, or non-obvious safety tradeoff should include a direct
comment explaining the reason.
