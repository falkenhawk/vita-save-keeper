# Google Drive setup

Save Keeper uploads backups to your own Google Drive. For that, the app must be registered
under a Google Cloud project that belongs to you. You do this once, it takes about ten minutes,
it is free, and no credit card or billing account is needed.

## Step 1 - create the project

1. open https://console.cloud.google.com/ and log in with your Google account
2. click the project picker in the top bar, then click **New project**
3. name it `PSV Save Keeper`, leave everything else as is, click **Create**
4. make sure the top bar now shows `PSV Save Keeper` as the active project

## Step 2 - enable the Google Drive API

1. open https://console.cloud.google.com/apis/library
2. type `Google Drive API` in the search box and click the result
3. click **Enable**

## Step 3 - set up the OAuth consent screen

1. open https://console.cloud.google.com/apis/credentials/consent
   (Google sometimes calls this page "OAuth consent screen" and sometimes "Google Auth Platform" -
   it is the same thing)
2. if asked for a user type, pick **External** and click **Create**
3. fill in only the required fields:
   - app name: `PSV Save Keeper`
   - user support email: your email
   - developer contact email: your email
4. click **Save and continue** through the remaining screens - do not add anything on the
   Scopes or Test users screens, they are not needed
5. back on the consent screen page, find **Publishing status** and click **Publish app**,
   then confirm. The status must say **In production**.

Why publishing matters: while the status is "Testing", Google deletes the sign-in after 7 days
and the Vita would ask you to reconnect every week. In production the sign-in lasts for years,
until you revoke it yourself. Save Keeper only asks for the `drive.file` permission (access to
files it created itself), which Google classifies as non-sensitive, so publishing does not
require any review or verification. Ignore any warning about verification - it does not apply
to non-sensitive permissions.

## Step 4 - create the client credentials

1. open https://console.cloud.google.com/apis/credentials
2. click **Create credentials**, then **OAuth client ID**
3. application type: **TVs and Limited Input devices** - this exact type is required, it is what
   enables the QR-code sign-in flow
4. name: `Save Keeper Vita` (only visible to you), click **Create**
5. a dialog shows the **Client ID** (ends in `.apps.googleusercontent.com`) and the
   **Client secret** (starts with `GOCSPX-`). Copy both.

## Step 5 - give the credentials to Save Keeper

Pick one of the two options.

Option A - a file on the memory card (no rebuild needed). Create
`ux0:data/save-keeper/google-client.json` with this exact content, using your two values:

```json
{"client_id": "xxxxx.apps.googleusercontent.com", "client_secret": "GOCSPX-xxxxx"}
```

Option B - baked into the VPK at build time:

```sh
cmake -S . -B build/vita -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DSAVE_KEEPER_GOOGLE_CLIENT_ID="xxxxx.apps.googleusercontent.com" \
  -DSAVE_KEEPER_GOOGLE_CLIENT_SECRET="GOCSPX-xxxxx"
cmake --build build/vita
```

## Step 6 - connect on the Vita

1. open Save Keeper and hold **Triangle**
2. scan the QR code with your phone - the sign-in code is already filled in. Or open
   `google.com/device` in any browser and type the code shown on the Vita
3. approve access on the phone. The Vita notices by itself within a few seconds - done.

The sign-in is stored in `ux0:data/save-keeper/google-token.json` and survives app exits,
reboots, and reinstalling the VPK. Treat that file like a password: anyone who copies it can
read the backups in your Drive.

More devices: copy the same `google-client.json` to each Vita / PS TV (or use the same VPK) and
run the sign-in once per device. All devices then share the same `PSV Saves` folder in your
Drive, which is what makes save sync between them work.

## Good to know

- Save Keeper can only see files it created itself - it cannot read anything else in your Drive
- backups live under the `PSV Saves` folder, one subfolder per game
- backups stay in your Drive even if you uninstall Save Keeper or wipe the Vita - you can browse
  and download the ZIPs from the Drive web UI on any computer
- if the Vita asks you to reconnect every week, the consent screen is still in Testing - publish
  it to **Production** (Step 3)
- to disconnect a device: delete `google-token.json` on it, and revoke the grant at
  https://myaccount.google.com/permissions
