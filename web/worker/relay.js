// emu8 — "send from PC" relay. The phone opens a room under a short code (role=recv) and a desktop
// page (/send) joins it (role=send); this Durable Object only forwards WebSocket messages between the
// two sides. Nothing is stored: a file exists only in flight, and the room is gone when both close.
//
//   GET /api/relay/<CODE>?role=recv|send   (WebSocket upgrade)
//
// It also carries the PC's web traffic (pc_net.cpp under EMU_WEB) to Protoweb's proxies, and only
// to them, so this can't be used as an open proxy:
//
//   GET /api/tcp?host=wayback.protoweb.org&port=7851   (WebSocket upgrade; binary frames are the
//                                                       stream; the page's text frame "shut", a FIN, is ignored)
//
// Everything else falls through to the static assets (dist/).

import { connect } from 'cloudflare:sockets';

// netmenu.cpp's kSpeeds (Protoweb's simulated lines) and Protoweb 2's one port.
const TCP_ALLOW = {
  'wayback.protoweb.org': [7851, 7809, 7814, 7819, 7828, 7833, 7856, 7864, 7128, 7256],
  'wayback2.protoweb.org': [7851],
};

const CODE_RE = /^[A-HJ-NP-Z2-9]{5}$/;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/api/tcp') return tcpTunnel(req, url);
    const m = url.pathname.match(/^\/api\/relay\/([^/]+)$/);
    if (!m) return env.ASSETS.fetch(req);
    const code = m[1].toUpperCase();
    const role = url.searchParams.get('role');
    if (!CODE_RE.test(code) || (role !== 'recv' && role !== 'send')) return new Response('bad request', { status: 400 });
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
    return env.RELAY.get(env.RELAY.idFromName(code)).fetch(req);
  },
};

export class Relay {
  constructor(ctx) { this.ctx = ctx; }

  async fetch(req) {
    const role = new URL(req.url).searchParams.get('role');
    const [client, server] = Object.values(new WebSocketPair());
    if (role === 'recv') {
      // one phone per code: a newer one (page reloaded) replaces the old
      for (const ws of this.ctx.getWebSockets('recv')) ws.close(4000, 'replaced');
      this.ctx.acceptWebSocket(server, ['recv']);
      for (const ws of this.ctx.getWebSockets('send')) safeSend(ws, '{"t":"peer","on":true}');
    } else {
      this.ctx.acceptWebSocket(server, ['send']);
      const on = this.ctx.getWebSockets('recv').length > 0;
      safeSend(server, JSON.stringify({ t: 'peer', on }));
      if (on) for (const ws of this.ctx.getWebSockets('recv')) safeSend(ws, '{"t":"peer","on":true}');
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, msg) {
    const other = this.ctx.getTags(ws)[0] === 'recv' ? 'send' : 'recv';
    for (const peer of this.ctx.getWebSockets(other)) safeSend(peer, msg);
  }

  webSocketClose(ws, code) {
    const role = this.ctx.getTags(ws)[0];
    const other = role === 'recv' ? 'send' : 'recv';
    const left = this.ctx.getWebSockets(role).filter((w) => w !== ws).length;
    if (!left) for (const peer of this.ctx.getWebSockets(other)) safeSend(peer, '{"t":"peer","on":false}');
    try { ws.close(code === 1005 ? 1000 : code); } catch (e) {}
  }

  webSocketError(ws) { this.webSocketClose(ws, 1011); }
}

function safeSend(ws, msg) { try { ws.send(msg); } catch (e) {} }

// One guest TCP connection: a WebSocket from the page, a socket to the proxy.
function tcpTunnel(req, url) {
  const host = (url.searchParams.get('host') || '').toLowerCase();
  const port = Number(url.searchParams.get('port'));
  if (!(TCP_ALLOW[host] || []).includes(port)) return new Response('not allowed', { status: 403 });
  if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
  const [client, ws] = Object.values(new WebSocketPair());
  ws.accept();
  let sock;
  try { sock = connect({ hostname: host, port }); } catch (e) { ws.close(1011, 'connect failed'); return new Response(null, { status: 101, webSocket: client }); }
  const writer = sock.writable.getWriter();
  let done = false;
  const end = (code, why) => {
    if (done) return;
    done = true;
    try { ws.close(code, why); } catch (e) {}
    try { sock.close(); } catch (e) {}
  };
  // page -> proxy, in order (each write waits for the one before)
  let chain = sock.opened.then(() => safeSend(ws, 'open'), () => end(1011, 'connect failed'));
  ws.addEventListener('message', (ev) => {
    chain = chain.then(() => {
      if (done) return;
      // 'shut' (the guest's FIN) is ignored: a half-close on Cloudflare, even with allowHalfOpen, left
      // Protoweb with nothing to answer (wrangler dev was fine). HTTP needs none: the proxy closes after
      // its answer, and that EOF reaches the guest.
      if (typeof ev.data === 'string') return undefined;
      // binary frames can come as a Blob (seen under wrangler dev) or an ArrayBuffer
      if (ev.data instanceof Blob) return ev.data.arrayBuffer().then((b) => writer.write(new Uint8Array(b)));
      return writer.write(new Uint8Array(ev.data));
    }).catch(() => end(1011, 'write failed'));
  });
  ws.addEventListener('close', () => end(1000));
  ws.addEventListener('error', () => end(1011));
  // proxy -> page; the proxy's FIN closes the WebSocket
  (async () => {
    const reader = sock.readable.getReader();
    try {
      for (;;) {
        const { value, done: eof } = await reader.read();
        if (eof || done) break;
        ws.send(value);
      }
      end(1000, 'eof');
    } catch (e) {
      end(1011, 'read failed');
    }
  })();
  return new Response(null, { status: 101, webSocket: client });
}
