# Google Drive setup

Save Keeper stores backups in your own Google Drive, so it needs OAuth client credentials that
belong to you. This is a one-time setup of about ten minutes.

## 1. Create the OAuth client

1. open https://console.cloud.google.com/ and create a project (any name, e.g. "Save Keeper")
2. enable the Drive API: APIs & Services > Library > search "Google Drive API" > Enable
3. configure the consent screen: APIs & Services > OAuth consent screen
   - user type: External
   - app name and your email are enough; no scopes need to be added on this screen
4. publish the app: on the OAuth consent screen page, set publishing status to **In production**.
   this matters: while the status is "Testing", Google expires refresh tokens after 7 days and the
   Vita would ask you to sign in again every week. In production the sign-in lasts until you revoke
   it yourself. Save Keeper only uses the `drive.file` scope, which is non-sensitive, so production
   publishing does not require Google's verification review.
5. create the client: APIs & Services > Credentials > Create credentials > OAuth client ID
   - application type: **TVs and Limited Input devices** (required for the QR/code sign-in flow)
6. copy the client ID and client secret

## 2. Give the credentials to Save Keeper

Option A - embed them into the VPK at build time:

```sh
cmake -S . -B build/vita -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DSAVE_KEEPER_GOOGLE_CLIENT_ID="your-id.apps.googleusercontent.com" \
  -DSAVE_KEEPER_GOOGLE_CLIENT_SECRET="your-secret"
cmake --build build/vita
```

Option B - put a JSON file on the memory card at `ux0:data/save-keeper/google-client.json`:

```json
{"client_id": "your-id.apps.googleusercontent.com", "client_secret": "your-secret"}
```

the client JSON downloaded from the Google console also works as-is; Save Keeper reads the
`client_id` and `client_secret` fields wherever they appear in the file.

## 3. Connect on the Vita

1. press Triangle in Save Keeper
2. scan the QR code with your phone - the sign-in code arrives pre-filled; or visit
   `google.com/device` in any browser and type the code shown on the Vita screen
3. approve access on the phone; the Vita polls Google automatically and finishes by itself

the sign-in is cached in `ux0:data/save-keeper/google-token.json` and survives app exits, reboots,
and firmware updates. Treat that file as a password: anyone who copies it can reach the backups in
your Drive.

To use several devices (Vita, PS TV), reuse the same client credentials on each one and run the
sign-in once per device. Google allows up to 100 active sign-ins per account for one client.

## Notes

- Save Keeper can only see files it created itself (the `drive.file` scope); backups live under a
  "PSV Saves" folder in your Drive
- to disconnect a device, delete `google-token.json` on it and revoke the grant at
  https://myaccount.google.com/permissions
