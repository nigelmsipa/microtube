import { Innertube, Platform } from 'youtubei.js';

// youtubei.js v17+ AST decipher shim (Node.js needs an explicit evaluator)
Platform.shim.eval = async (data, env) => {
  const properties = [];
  if (env.n) properties.push(`n: exportedVars.nFunction("${env.n}")`);
  if (env.sig) properties.push(`sig: exportedVars.sigFunction("${env.sig}")`);
  const code = `${data.output}\nreturn { ${properties.join(', ')} }`;
  return new Function(code)();
};

const rawQuery = process.argv[2];
const parameters = JSON.parse(process.argv[3]);

// youtubei.js v17 with the ANDROID_VR client throws 'This video is unavailable'
// when given a full youtube.com URL — it only accepts bare 11-char video IDs.
// (The IOS client accepted both forms, which is why this used to work.) The
// download path in videodownloader.cpp passes a URL; playback passes an ID.
// Normalize here so both callers work.
function extractVideoId(q) {
  if (/^[\w-]{11}$/.test(q)) return q;
  try {
    const u = new URL(q);
    const v = u.searchParams.get('v');
    if (v) return v;
    // youtu.be/<id> or /shorts/<id> or /embed/<id>
    const m = u.pathname.match(/\/(?:shorts|embed)\/([\w-]{11})/) ||
              u.pathname.match(/^\/([\w-]{11})$/);
    if (m) return m[1];
  } catch (_) {}
  return q;
}
const query = extractVideoId(rawQuery);

// PO token + synchronized visitor_data (from fetchPOToken.js) un-redacts the
// stream payload; retrieve_player lets youtubei.js stamp/decipher stream URLs.
const youtube = await Innertube.create({
  lang: parameters.language,
  location: parameters.country,
  enable_safety_mode: parameters.safeSearch,
  po_token: parameters.poToken.poToken,
  visitor_data: parameters.poToken.visitorData,
  generate_session_locally: true,
  retrieve_player: true,
});

const info = await youtube.getInfo(query, { client: 'ANDROID_VR' });

const formats = [
  ...(info?.streaming_data?.formats || []),
  ...(info?.streaming_data?.adaptive_formats || [])
];

// Decipher every format (await — async in v16+) so the C++ side gets real URLs.
if (!youtube.session.player) {
  process.stderr.write('videoInfo: WARNING player is null — n-transform will not fire, downloads will throttle\n');
}
function getUrlParam(url, key) {
  if (!url) return null;
  try {
    return new URL(url).searchParams.get(key);
  } catch (_) {
    return null;
  }
}
const urls = [];
for (const format of formats) {
  const origUrl = format.url;
  const origN = getUrlParam(origUrl, 'n');
  try {
    format.url = await format.decipher(youtube.session.player);
    const newN = getUrlParam(format.url, 'n');
    if (origN && newN === origN) {
      process.stderr.write('videoInfo: decipher left n unchanged for itag ' + format.itag + ' — n-transform may have failed\n');
    }
    if (format.url && !getUrlParam(format.url, 'pot')) {
      process.stderr.write('videoInfo: deciphered URL has no pot for itag ' + format.itag + '\n');
    }
  } catch (e) {
    process.stderr.write('videoInfo: decipher error itag ' + format.itag + ': ' + e.message + '\n');
  }
  urls.push(format);
}

console.log(JSON.stringify({ info: info, formats: urls }, null, 2));
