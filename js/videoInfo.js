import { Innertube, Platform } from 'youtubei.js';

// youtubei.js v17+ AST decipher shim (Node.js needs an explicit evaluator)
Platform.shim.eval = async (data, env) => {
  const properties = [];
  if (env.n) properties.push(`n: exportedVars.nFunction("${env.n}")`);
  if (env.sig) properties.push(`sig: exportedVars.sigFunction("${env.sig}")`);
  const code = `${data.output}\nreturn { ${properties.join(', ')} }`;
  return new Function(code)();
};

const query = process.argv[2];
const parameters = JSON.parse(process.argv[3]);

// PO token + synchronized visitor_data (from fetchPOToken.js) un-redacts the
// stream payload; retrieve_player + the iOS client return decipherable URLs.
const youtube = await Innertube.create({
  lang: parameters.language,
  location: parameters.country,
  enable_safety_mode: parameters.safeSearch,
  po_token: parameters.poToken.poToken,
  visitor_data: parameters.poToken.visitorData,
  generate_session_locally: true,
  retrieve_player: true,
});

const info = await youtube.getInfo(query, { client: 'IOS' });

const formats = [
  ...(info?.streaming_data?.formats || []),
  ...(info?.streaming_data?.adaptive_formats || [])
];

// Decipher every format (await — async in v16+) so the C++ side gets real URLs.
const urls = [];
for (const format of formats) {
  try {
    if (!format.url) format.url = await format.decipher(youtube.session.player);
  } catch (e) { /* keep whatever url it has */ }
  urls.push(format);
}

console.log(JSON.stringify({ info: info, formats: urls }, null, 2));
