# Changelog

All notable changes to Save Keeper are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] - unreleased

### Added
- new snapshots use the latest actual Vita save-slot date in their filename. If a game does not
  expose slot data, Save Keeper falls back to the newest save file time and finally the backup time
- hold Triangle on a snapshot to open a full-screen slot browser with each slot's date, title,
  subtitle, and description
- optional JSON metadata companions sync with each ZIP on Google Drive. They are loaded only when
  details are requested, so the normal backup index stays lightweight
- back up and upload a whole tab in one gesture: hold Select to snapshot every game in the
  current Vita / Homebrew / PSP tab and send it to the Cloud. Ideal for a first full backup of
  your library - later runs only upload the backups that are new, not everything again
- backup labels: give a snapshot a name ("before boss", "100% save") with the on-screen
  keyboard. The label is added after the timestamp, so backups still sort by date, and a label
  edit is mirrored to the Cloud copy

### Changed
- existing 1.0 ZIP-only backups remain fully usable and are not renamed. For a local Vita backup,
  slot details are recovered lazily from the ZIP without extracting or rewriting it
- missing, malformed, or failed metadata never blocks ZIP backup, upload, download, restore,
  labeling, or deletion; whole-save ZIPs remain the only backup and restore boundary
- one row per snapshot instead of separate local and Cloud entries, with a small cloud glyph
  marking where it lives: on the memory card, in the Cloud, or both
- download a Cloud-only backup to the memory card without restoring it (Select)
- when a backup exists both on the card and in the Cloud, deleting it asks where: local only,
  Cloud only, or both
- Drive folders now carry the game's title (for example `PCSB00456 FEZ`), so backups are
  browsable in the Google Drive web UI
- footer button hints, hold-gesture cues, and status messages cleaned up throughout

## [1.0.0] - 2026-07-05

Initial release: timestamped ZIP snapshots of Vita, homebrew, and PSP saves; upload, download,
and restore through your own Google Drive; QR-code sign-in; a game grid with real titles and
icons; sorting by name, last saved, or last synced; and an automatic pre-restore snapshot so a
restore never destroys the current save.
