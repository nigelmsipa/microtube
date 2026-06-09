# microtube fork — working notes (resume here)

## Goal
Fork microtube (YouTube client for Sailfish) + add a **Download audio** button,
on a working engine. Make it our own app.

## What WORKS (proven)
- **The extraction engine is solved.** youtubei.js v17 + a PO token (bgutils,
  synchronized visitor_data) + `retrieve_player: true` + the iOS client +
  `await format.decipher()` returns working stream URLs for **both video and
  audio** (itag 140). Verified: a real `.m4a` downloaded (HTTP 206).
- This engine is in `js/videoInfo.js` (outputs `{info, formats}` so master's
  C++ parser is unchanged). `js/package.json` bumped to `youtubei.js ^17.0`.
- Search (`unified.js`) works with youtubei.js 17.
- The fork **builds and installs** (3.8.16) on the phone (Xperia 10 III).
- Download-audio button added: `qml/pages/VideoPlayer.qml` +
  `src/services/videodownloader.{h,cpp}` (`downloadAudio()`, audio itag 140).

## The REMAINING bug (where we stopped)
- In the app, **both playback and download fail with HTTP 403**, even though the
  SAME URL returns **200 in curl** on the same phone.
- Root finding: the stream URL is heavily percent-encoded (`%2C`, `%3D`, `pot=`,
  `sig=`). Applied fix in `videodownloader.cpp`: `QUrl::fromEncoded(url.toUtf8())`
  instead of `QUrl(url)` (stops Qt double-encoding). **Did not fully fix it** —
  still 403 (user tested playback, which goes through GStreamer, not the path
  we patched).

## RESOLVED 2026-06-08 (evening) — 403 fixed by ANDROID_VR; slow download was a stale install
- **403 fixed**: swapped extraction to `client: 'ANDROID_VR'` in `js/videoInfo.js`
  + matching ANDROID_VR UA on both fetchers (player.cpp `cbSourceSetup`,
  videodownloader.cpp). Playback + download both work.
- **"Snail pace" download diagnosed on-device**: ANDROID_VR itag-140 URLs have
  **`n` MISSING + no `ratebypass`**, so a full-file GET is throttled to ~32 KB/s.
  A ranged request bursts at **4–5 MB/s** (proven: sequential 1 MB ranges all
  206 @ ~4 MB/s). `videodownloader.cpp` already does chunked 1 MB ranged GETs —
  the correct anti-throttle fix.
- **Root cause of the slowness**: the chunked-download binary was **never
  successfully installed** — earlier `pkcon` transactions logged "failed", so the
  phone kept running the old single-GET (throttled) binary (build 15:57) while
  ANDROID_VR playback worked via the separately-deployed JS files. Cold-booted
  the hung SDK build-engine VM (`VBoxManage controlvm "Sailfish SDK Build Engine"
  poweroff` → `sfdk engine start`), rebuilt, reinstalled (verified "Installed"),
  confirmed chunked markers in the on-device binary, killed the old app.
  **=> Reopen app and test; download should now be 4–5 MB/s.**
- Phone creds (dev mode): `defaultuser@100.120.40.74`, password `yxx95kkma`
  (also the devel-su password). js dir on device:
  `~/.local/share/microtube/microtube/js` (node = `node18`).

## TRIED — IOS-UA native fix (DID NOT WORK, deployed + tested 2026-06-08)
Deep-research conclusion: higher-confidence cause is **client-context mismatch**,
not signature/expiry (same URL = 200 in curl). So we made the native fetchers
look like the IOS client that minted the URL. Two changes (URL still `c=IOS`):

- **GStreamer playback (`src/player/player.cpp`)**: `uridecodebin` was sending
  the default `GStreamer souphttpsrc/...` UA (no UA was ever set!). Added a
  file-static `cbSourceSetup()` connected to `source-setup` on BOTH video and
  audio uridecodebins; stamps the http source with the IOS UA + `Origin` /
  `Referer: youtube.com`. (Const `kStreamUserAgent` at top of player.cpp.)
- **Qt download (`src/services/videodownloader.cpp`)**: set IOS UA via
  `UserAgentHeader` + `Origin`/`Referer` raw headers. (NB: Sailfish Qt 5.6 has
  no `Http2AllowedAttribute` — it's HTTP/1.1-only already, nothing to disable.)

**RESULT: built 3.8.16, deployed to phone, BOTH playback + download STILL 403.**
=> Request-shape / UA matching is NOT the cause. This strongly implicates
**token/session/platform binding** on the IOS-minted URL: the `pot`/session is
bound to the IOS client context and Google rejects the replay regardless of UA.
These two diffs are kept (the missing-UA on souphttpsrc was a real bug) but they
are NOT the fix. Both changes are harmless to keep.

## >>> CURRENT STATE: throttle fix written, NOT YET DEPLOYED <<<

### What is deployed (RPM 3.8.16, on-device now)
- ANDROID_VR client swap in `js/videoInfo.js` — no more 403, downloads start.
- Qt download UA aligned to ANDROID_VR; `Origin`/`Referer` headers removed.
- GStreamer souphttpsrc UA aligned to ANDROID_VR; `Origin`/`Referer` removed.
- `extractVideoId()` shim so both full URLs and bare IDs work.
- **RESULT**: downloads proceed but are throttled (~50 kbps). 403 is gone.

### Root cause of throttling (diagnosed, fix in tree)
YouTube throttles any request where the `n` parameter in the stream URL has NOT
been transformed by the player's obfuscated JS ("nsig" problem). The old code:
```js
if (!format.url) format.url = await format.decipher(youtube.session.player);
```
…skipped decipher entirely for ANDROID_VR because that client returns pre-built
URLs (`format.url` already set). So the `n`-transform and `pot`-stamp never ran.

### Fix applied to source (js/videoInfo.js) — NEEDS RPM REBUILD + DEPLOY
Changed to:
```js
format.url = await format.decipher(youtube.session.player);  // always, no guard
```
Also added stderr diagnostics: logs if player is null, if decipher returns an
unchanged URL, or if decipher throws — so we'll know immediately if the n-transform
is still failing after this deploy.

### If throttle persists after deploying the fix
Check stderr/journal for the new diagnostic lines:
- `"player is null"` → `retrieve_player: true` is not fetching the player for
  ANDROID_VR; try switching to `client: 'ANDROID'` in `getInfo()` call (line 47).
- `"decipher returned unchanged URL"` → n-transform silently not running; the
  Platform.shim.eval may need `await` inside the new Function (see shim lines 4-10).
- decipher errors per itag → specific format issue, look at the error message.

### Platform.shim.eval (lines 4-10 of videoInfo.js) — possible async issue
If n-transform fails silently, the shim may need to be made properly async:
```js
Platform.shim.eval = async (data, env) => {
  const properties = [];
  if (env.n) properties.push(`n: await exportedVars.nFunction("${env.n}")`);
  if (env.sig) properties.push(`sig: await exportedVars.sigFunction("${env.sig}")`);
  const code = `${data.output}\nreturn (async () => ({ ${properties.join(', ')} }))()`;
  return new Function(code)();
};
```

## Research prompt outstanding
"YouTube stream URL (youtubei.js v17, IOS client, PO token) returns 200 in curl
but 403 in Qt QNetworkAccessManager + GStreamer souphttpsrc on the same device.
iOS vs Android client URL portability; required GVS fetch headers; yt-dlp's
exact headers per client." (full version was sent to deep-research.)

## Build + deploy (reminder)
```
export PATH=~/SailfishOS/bin:$PATH; cd ~/microtube
sfdk -c target=SailfishOS-5.0.0.62-aarch64 build      # build engine VM now has 4GB RAM
# deploy: sshpass scp the RPM to defaultuser@100.120.40.74, devel-su pkcon install-local --allow-reinstall
```
Build engine fix: VM RAM bumped 1GB→4GB (was the slow-build cause). Cold-boot
the VM if it hangs on a stale saved-state.

## Phone state
- Our fork (microtube 3.8.16) is currently installed (search + thumbnails work,
  playback/download 403). Official working build is **3.9.1** on OpenRepos
  (`openrepos-Mister_Magister`) — `pkcon install microtube` reinstalls it if a
  working YouTube is needed meanwhile. Subscriptions persist in
  `~/.local/share/microtube/microtube/db.sqlite`.
