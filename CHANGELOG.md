# Changelog

All notable changes to Save Keeper are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] - unreleased

### Added
- back up and upload a whole tab in one gesture: hold Select to snapshot every game in the
  current Vita / Homebrew / PSP tab and sync each to Drive, skipping anything already backed up
- backup labels: give a snapshot a name ("before boss", "100% save") with the on-screen
  keyboard. The label is added after the timestamp, so backups still sort by date, and a label
  edit is mirrored to the copy on Drive
- one row per snapshot instead of separate local and Drive entries, with a small cloud glyph
  marking where it lives: on the memory card, on Google Drive, or both
- download a Drive-only backup to the memory card without restoring it (Select)
- when a backup exists on both the card and Drive, deleting it asks where: card only, Drive
  only, or both
- Drive folders now carry the game's title (for example `PCSB00456 FEZ`), so backups are
  browsable in the Google Drive web UI

### Changed
- redesigned the LiveArea start card: the cloud sync glyph replaces the old decorative tick,
  the tagline is set in a cleaner face, and the whole lockup is centered
- uploads skip a backup already on Drive by matching its timestamp, so a labeled or renamed
  backup is never uploaded twice
- long backup names scroll to reveal themselves when selected instead of being cut off
- footer button hints, hold-gesture cues, and status messages cleaned up throughout

### Fixed
- a failed Drive download no longer leaves a partial ZIP behind that looked like a real backup

## [1.0.0] - 2026-07-05

Initial release: timestamped ZIP snapshots of Vita, game card, and PSP saves; upload, download,
and restore through your own Google Drive; QR-code sign-in; a game grid with real titles and
icons; sorting by name, last saved, or last synced; and an automatic pre-restore snapshot so a
restore never destroys the current save.
