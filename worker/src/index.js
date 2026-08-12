// Storage proxy for Hare's cloud sync.
//
// Its reason to exist is setup cost: reaching R2 directly asks the user for an
// endpoint, a bucket name, an access key and a secret. Behind this Worker the
// same thing takes a URL and a token, and the R2 credentials never leave
// Cloudflare.
//
// The snapshots arrive already encrypted with a key derived on the user's
// machine, so this code — and Cloudflare with it — only ever handles ciphertext.
//
// Endpoints, all requiring `Authorization: Bearer <SYNC_TOKEN>`:
//
//   GET  /list        JSON array of object names
//   GET  /o/<name>    object bytes
//   PUT  /o/<name>    store object bytes
//
// Object names look like "<installation_id>/<file>.userdb.txt", plus the single
// "keys/dek.bin" holding the wrapped data key.

const PREFIX = 'hare/';

function unauthorized() {
  return new Response('unauthorized', { status: 401 });
}

// Compares in constant time so a token cannot be recovered by timing repeated
// requests.
function tokenMatches(provided, expected) {
  if (typeof provided !== 'string' || provided.length !== expected.length) {
    return false;
  }
  let difference = 0;
  for (let i = 0; i < provided.length; i++) {
    difference |= provided.charCodeAt(i) ^ expected.charCodeAt(i);
  }
  return difference === 0;
}

// Keeps a request from reaching outside the prefix, whatever the client sends.
function objectKey(name) {
  const decoded = decodeURIComponent(name);
  if (!decoded || decoded.includes('..') || decoded.startsWith('/')) {
    return null;
  }
  return PREFIX + decoded;
}

export default {
  async fetch(request, env) {
    if (!env.SYNC_TOKEN) {
      return new Response('SYNC_TOKEN is not configured', { status: 500 });
    }

    const header = request.headers.get('Authorization') || '';
    if (!header.startsWith('Bearer ') ||
        !tokenMatches(header.slice(7), env.SYNC_TOKEN)) {
      return unauthorized();
    }

    const url = new URL(request.url);

    if (request.method === 'GET' && url.pathname === '/list') {
      const names = [];
      let cursor;
      do {
        const page = await env.SYNC_BUCKET.list({ prefix: PREFIX, cursor });
        for (const object of page.objects) {
          names.push(object.key.slice(PREFIX.length));
        }
        cursor = page.truncated ? page.cursor : undefined;
      } while (cursor);
      return Response.json(names);
    }

    if (url.pathname.startsWith('/o/')) {
      const key = objectKey(url.pathname.slice(3));
      if (!key) {
        return new Response('bad object name', { status: 400 });
      }

      if (request.method === 'GET') {
        const object = await env.SYNC_BUCKET.get(key);
        if (!object) {
          return new Response('not found', { status: 404 });
        }
        return new Response(object.body, {
          headers: { 'Content-Type': 'application/octet-stream' },
        });
      }

      if (request.method === 'PUT') {
        await env.SYNC_BUCKET.put(key, request.body);
        return new Response('ok');
      }
    }

    return new Response('not found', { status: 404 });
  },
};
