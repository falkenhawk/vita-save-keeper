# Save Keeper

Native PS Vita save backup and restore app with planned Google Drive snapshot support.

## Current status

This repository currently contains the first buildable foundation:

- host-tested core logic for JKSV-style timestamped backup names
- local/remote backup menu entries, with only remote entries shown as `[GD] ...`
- path component normalization for future local and Google Drive folder names
- startup discovery for Vita saves and PSP/Adrenaline saves
- local timestamped ZIP backup creation
- local backup listing for the selected save
- local backup restore with a second-press confirmation
- Google OAuth device-code request and token polling controls
- Google Drive upload for the selected local ZIP backup
- Google Drive listing and download-then-restore for `[GD]` ZIP backups
- a native `vita2d` UI preview that builds into a VPK

Delete/rename flows and QR rendering are not implemented yet.

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

## Save roots

The current foundation scans these roots at startup:

- `ux0:user/00/savedata`
- `ux0:pspemu/PSP/SAVEDATA`

Local backups are written under `ux0:data/save-keeper/backups`.

## Google auth

Development builds expect OAuth client credentials at
`ux0:data/save-keeper/google-client.json`. The JSON must contain `client_id` and `client_secret`
fields from a Google OAuth client of type TVs and Limited Input devices. Press `Triangle` once to
request a device code, finish consent on a phone or PC, then press `Triangle` again to poll and save
the token cache to `ux0:data/save-keeper/google-token.json`.

After auth, press `Triangle` to refresh remote backups for the selected save. Press `Select` to
upload the selected local backup. Press `Square` on a local backup to restore it, or on a `[GD]`
backup to download it locally and then restore it.

## Comment style

Straightforward code should stay straightforward. Logic that encodes a product decision, external
API behavior, Vita-specific constraint, or non-obvious safety tradeoff should include a direct
comment explaining the reason.
