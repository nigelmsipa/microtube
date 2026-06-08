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

## Leading hypothesis (next to try)
- **iOS-client stream URLs may be bound to the iOS client** → a generic native
  HTTP client (Qt/GStreamer) gets 403 while curl (in a controlled test) gets 200.
- **Try the ANDROID client** in `videoInfo.js` (`getInfo(query, {client:'ANDROID'})`)
  — Android URLs are usually NOT client-UA-bound and more portable to native
  fetchers. (Android needed the decipher; with PO token + await it should work.)
- Also: playback path is GStreamer `souphttpsrc` — may need a `user-agent` /
  extra-headers; download path is Qt `QNetworkAccessManager`.

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
