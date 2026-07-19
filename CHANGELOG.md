# Changelog

All notable changes to Save Keeper are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- huge backups no longer crash the app during Cloud upload. The upload used to load the whole ZIP
  into memory (and copy it while packing the request), which ran the Vita out of RAM on giant
  saves - a bloated LittleBigPlanet save weighs over 50 MB. Uploads now stream straight from the
  memory card, so backup size no longer matters
- the upload time limit now grows with the backup size, so a big save on slow wifi is not cut off
  mid-transfer
- if the app ever runs out of memory while uploading, it shows an error and carries on instead of
  crashing to the LiveArea

## [1.1.0] - 2026-07-19

### Added
- back up and upload a whole tab in one gesture: hold Select to back up every game in the current
  Vita / Homebrew / PSP tab and send it to Google Drive, later runs only process what is new
- full-screen save details on Triangle: every slot's date, title, subtitle, and description, plus
  the live save's size or a backup's ZIP file size. The overview's backup, restore, transfer, and
  label actions work right there on the inspected backup. Details load only when the view
  opens, so browsing stays instant
- backup labels: name a backup ("before boss", "100% save") with the on-screen keyboard. The
  label follows the timestamp so backups still sort by date, and label edits mirror to the Cloud
- real save times: the actual save-slot date is read from each game's save data and used for the
  grid's "Last save" line, the last-saved sort, and backup filenames, with file times as the
  fallback. Once read, times and titles are cached, so a launch only re-reads what changed
- download a Cloud-only backup without restoring it (Select)
- deleting a backup that exists on both sides asks where: local only, Cloud only, or both
- a Google Drive setup screen: trying to connect without the credentials file now explains what
  is missing and shows a QR code straight to the step-by-step guide
- each backup now carries a small JSON file with its slot details, stored next to the ZIP locally
  and on Google Drive. It is optional - a missing or broken one never gets in the way, and Save
  Keeper can fill in details for backups made by 1.0

### Changed
- every backup is one clean row: the separate local and `[GD]`-prefixed Cloud entries of 1.0 are
  merged, and a small cloud glyph on each row marks where it lives - locally, in the Cloud, or both
- sorting cycles name, latest backup, then last saved. Last saved now orders by real save times,
  and reading them shows a progress modal that Square cancels back to name order
- creating a backup when nothing changed since an existing one shows a "No changes" warning
  instead of writing a duplicate, a second press creates one anyway
- Drive folders now carry the game's title next to the ID (for example `PCSB00456 FEZ`), so
  backups are easier to recognize and find in the Google Drive web UI
- existing 1.0 backups remain fully usable and are not renamed
- backup names display without the .zip extension
- footer button hints, hold-gesture cues, and status messages cleaned up throughout
- refreshed app assets: new icon, LiveArea background, and start screen

## [1.0.0] - 2026-07-05

Initial release:

- timestamped ZIP backups of Vita, homebrew, and PSP saves
- upload, download, and restore through your own Google Drive
- QR-code sign-in
- a game grid with real titles and icons
- sorting by name, last saved, or last synced
- an automatic pre-restore backup, so a restore never destroys the current save
