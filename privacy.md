# Privacy policy

Last updated: 24 September 2026

Save Keeper has no servers and collects nothing. Your saves go from your console straight to your own
Google Drive, and nobody else, the developer included, ever receives them.

## What the app accesses

On your console, Save Keeper reads the save folders of your Vita, PSP/Adrenaline and homebrew games to back
them up, and the list of installed apps to show game titles and icons.

In your Google account, it asks for one permission, `drive.file`, which covers only files the app created
itself: a `PSV Saves` folder with one folder per game, your backup ZIPs with a small info file each, and
`backup-settings.json` with the homebrew folders you chose to back up. It cannot see any other file in your
Drive and does not ask for your name, email address or any other Google data.

## Where your data goes

Your console talks directly to Google over HTTPS. There is no Save Keeper server, and no analytics,
telemetry, crash reporting, advertising or tracking. Nothing is shared with or sold to anyone.

Save Keeper keeps its own files, including local backups and your Google sign-in token, in
`ux0:data/save-keeper` on your memory card. Treat `google-token.json` like a password.

## Deleting your data

Backups stay in your Drive until you delete them, also after you uninstall Save Keeper. To disconnect it,
remove its access at [myaccount.google.com/permissions](https://myaccount.google.com/permissions) and delete
`google-token.json`. To remove everything, also delete the `PSV Saves` folder from your Drive and
`ux0:data/save-keeper` from your console.

## Google API Services User Data Policy

Save Keeper's use and transfer of information received from Google APIs adheres to the
[Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy),
including the Limited Use requirements.

## Contact

Questions go to the [issue tracker](https://github.com/falkenhawk/vita-save-keeper/issues). Changes to this
policy are published on this page, and its history is
[public on GitHub](https://github.com/falkenhawk/vita-save-keeper/commits/gh-pages/privacy.md).
