# Google Drive setup

Save Keeper uploads backups to your own Google Drive. For that, the app must be registered
under a Google Cloud project that belongs to you. You do this once, it takes about ten minutes,
it is free, and no credit card or billing account is needed.

## Step 1 - create the project

1. open https://console.cloud.google.com/projectcreate and log in with your Google account
2. if this is your first time in Google Cloud, a welcome dialog appears. Tick the terms of service
   and click **Agree and continue**. The email updates box can stay empty
3. set **Project name** to `PSV Save Keeper`, leave everything else as is, click **Create**
4. make sure the top bar now shows `PSV Save Keeper` as the active project

Ignore the "Start your free trial" banner at the top of the page, Save Keeper does not need it.

## Step 2 - enable the Google Drive API

1. open https://console.cloud.google.com/apis/library/drive.googleapis.com
2. click **Enable**

## Step 3 - set up the OAuth consent screen

1. open https://console.cloud.google.com/auth/overview and click **Get started**
   (Google sometimes calls this page "OAuth consent screen" and sometimes "Google Auth Platform" -
   it is the same thing)
2. fill in the wizard, clicking **Next** after each section:
   - **App name**: `PSV Save Keeper`
   - **User support email**: pick your email
   - **Audience**: **External**
   - **Contact Information**: your email
3. in the **Finish** section tick the agreement, click **Continue**, then **Create**
4. open **Branding** in the left panel, fill in these three fields and click **Save**.
   Leave the logo and the terms of service link empty:
   - **Application home page**: `https://falkenhawk.github.io/vita-save-keeper/`
   - **Application privacy policy link**: `https://falkenhawk.github.io/vita-save-keeper/privacy.html`
   - **Authorized domains**: click **Add domain** and enter `falkenhawk.github.io`
5. *(probably optional, but recommended)*\
   open **Data access** in the left panel and click **Add or remove scopes**. Paste
   `https://www.googleapis.com/auth/drive.file` into **Manually add scopes**, click **Add to table**,
   then **Update**, then **Save**
6. open **Audience** in the left panel, click **Publish app**, then **Confirm**.
   The status must say **In production**.

The two links above are Save Keeper's own pages, so you do not need a website or a domain of your own.

After publishing, Google shows a yellow "Your branding needs to be verified" warning with a
**Verify branding** button. Ignore it. That check needs a domain you own, and it is not needed here:
your app stays in production and the sign-in works without it.

Why publishing matters: while the status is "Testing", Google deletes the sign-in after 7 days
and the Vita would ask you to reconnect every week. In production the sign-in keeps working
until you revoke it, or until it goes unused for six months. Save Keeper only asks for the `drive.file` permission (access to
files it created itself), which Google classifies as non-sensitive, so publishing does not
require any review or verification. Ignore any warning about verification - it does not apply
to non-sensitive permissions.

## Step 4 - create the client credentials

1. open https://console.cloud.google.com/auth/clients/create
2. **Application type**: **TVs and Limited Input devices** - this exact type is required, it is what
   enables the QR-code sign-in flow
3. **Name**: `Save Keeper Vita` (only visible to you), click **Create**
4. the **OAuth client created** dialog shows the **Client ID** (ends in `.apps.googleusercontent.com`)
   and the **Client secret** (starts with `GOCSPX-`). Copy both now, Google will not show the secret
   again after you close this dialog.

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
3. on the phone, pick your account and click **Continue**. The Vita notices by itself within a few
   seconds - done.

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
- if **Publish app** is greyed out, the three Branding fields from Step 3 are not saved yet
- if the Vita asks you to reconnect every week, the consent screen is still in Testing - publish
  it (Step 3) and sign in again
- to disconnect a device: delete `google-token.json` on it, and revoke the grant at
  https://myaccount.google.com/permissions
