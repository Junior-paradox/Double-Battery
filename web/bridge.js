/* HealthWay web bridge.
 *
 * Sits between the browser and the C dispatch daemon. Zero npm dependencies:
 * the WebSocket handshake and framing are implemented directly against
 * node:crypto so this runs on a bare Node install with nothing to `npm i`.
 *
 * The engine link is a POOL of pipelined TCP connections. A single
 * un-pipelined client tops out around 3.6k req/s bound by round-trip latency,
 * while the same engine does 42k/s pipelined across a few connections -- so
 * the bridge never issues one blocking call per emergency.
 */
const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ENGINE_HOST = '127.0.0.1';
const ENGINE_PORT = Number(process.env.ENGINE_PORT || 9090);
const HTTP_PORT = Number(process.env.PORT || 8080);
const POOL_SIZE = 4;

/* ---------------- engine connection pool ---------------- */
class EngineConn {
  constructor() {
    this.pending = [];
    this.buf = '';
    this.ready = new Promise((res, rej) => {
      this.sock = net.createConnection(ENGINE_PORT, ENGINE_HOST, res);
      this.sock.on('error', rej);
    });
    this.sock.setNoDelay(true);
    this.sock.setEncoding('utf8');
    this.sock.on('data', (chunk) => {
      this.buf += chunk;
      let i;
      while ((i = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, i);
        this.buf = this.buf.slice(i + 1);
        const resolve = this.pending.shift();
        if (!resolve) continue;
        try { resolve(JSON.parse(line)); }
        catch { resolve({ ok: false, error: 'bad json from engine' }); }
      }
    });
    this.sock.on('close', () => {
      this.pending.splice(0).forEach((r) => r({ ok: false, error: 'engine closed' }));
    });
  }
  /* Responses come back strictly in order, so a FIFO of resolvers is all the
     correlation this protocol needs -- no request ids on the wire. */
  send(cmd) {
    return new Promise((resolve) => {
      this.pending.push(resolve);
      this.sock.write(cmd + '\n');
    });
  }
}

class EnginePool {
  constructor(n) { this.conns = Array.from({ length: n }, () => new EngineConn()); this.i = 0; }
  ready() { return Promise.all(this.conns.map((c) => c.ready)); }
  send(cmd) { return this.conns[this.i++ % this.conns.length].send(cmd); }
  /* Fire a whole batch down one connection so it pipelines instead of
     paying a round trip per item. */
  batch(cmds) {
    const c = this.conns[this.i++ % this.conns.length];
    return Promise.all(cmds.map((x) => c.send(x)));
  }
}
const engine = new EnginePool(POOL_SIZE);

/* ---------------- websocket (RFC 6455 subset) ---------------- */
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const clients = new Set();

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}
function wsFrame(str) {
  const payload = Buffer.from(str, 'utf8');
  const len = payload.length;
  let head;
  if (len < 126) {
    head = Buffer.alloc(2); head[1] = len;
  } else if (len < 65536) {
    head = Buffer.alloc(4); head[1] = 126; head.writeUInt16BE(len, 2);
  } else {
    head = Buffer.alloc(10); head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2);
  }
  head[0] = 0x81;                       /* FIN + text opcode */
  return Buffer.concat([head, payload]);
}
function wsRead(buf, onText) {
  /* Returns the unconsumed remainder. Client frames are always masked. */
  let off = 0;
  for (;;) {
    if (buf.length - off < 2) break;
    const b0 = buf[off], b1 = buf[off + 1];
    const opcode = b0 & 0x0f, masked = (b1 & 0x80) !== 0;
    let len = b1 & 0x7f, p = off + 2;
    if (len === 126) { if (buf.length - p < 2) break; len = buf.readUInt16BE(p); p += 2; }
    else if (len === 127) { if (buf.length - p < 8) break; len = Number(buf.readBigUInt64BE(p)); p += 8; }
    if (buf.length - p < (masked ? 4 : 0) + len) break;
    let mask = null;
    if (masked) { mask = buf.subarray(p, p + 4); p += 4; }
    const body = Buffer.from(buf.subarray(p, p + len));
    if (mask) for (let i = 0; i < body.length; i++) body[i] ^= mask[i & 3];
    p += len;
    off = p;
    if (opcode === 0x8) return null;                 /* close */
    if (opcode === 0x1) onText(body.toString('utf8'));
  }
  return buf.subarray(off);
}
function broadcast(obj) {
  const frame = wsFrame(JSON.stringify(obj));
  for (const s of clients) { try { s.write(frame); } catch { clients.delete(s); } }
}

/* ---------------- simulation state ---------------- */
/* Capability bits, mirroring the enum in src/dispatch.h. */
const H = { TRAUMA:1, CARDIAC:2, NEURO:4, BURNS:8, OBSTETRIC:16, PAEDS:32, TOXICOL:64, ICU:128 };
const A = { ALS:256, VENT:512, NEONATAL:1024 };

/* Presenting complaint -> required destination capability and vehicle kit.
 * This is what makes the specialty routing mean something: a delivery will
 * not be taken to a hospital without obstetrics, and a neonatal-equipped
 * vehicle is required to carry it, no matter how close a plain van is. */
const MED = ['analgesic', 'adrenaline', 'anticoagulant', 'burn dressing',
             'oxytocin', 'paediatric AB', 'antivenom', 'sedative'];

const CASES = [
  { name: 'road accident',        hosp: H.TRAUMA,          amb: A.ALS | A.VENT, med: 0, qty: 2, u: 3, w: 18 },
  { name: 'cardiac arrest',       hosp: H.CARDIAC,         amb: A.ALS | A.VENT, med: 1, qty: 2, u: 3, w: 14 },
  { name: 'stroke',               hosp: H.NEURO,           amb: A.ALS,          med: 2, qty: 1, u: 3, w: 10 },
  { name: 'labour / delivery',    hosp: H.OBSTETRIC,       amb: A.NEONATAL,     med: 4, qty: 1, u: 2, w: 12 },
  { name: 'severe burns',         hosp: H.BURNS,           amb: A.ALS,          med: 3, qty: 3, u: 3, w: 5  },
  { name: 'poisoning',            hosp: H.TOXICOL,         amb: A.ALS,          med: 6, qty: 2, u: 2, w: 7  },
  { name: 'paediatric emergency', hosp: H.PAEDS,           amb: 0,              med: 5, qty: 1, u: 1, w: 12 },
  { name: 'eye injury',           hosp: H.TRAUMA,          amb: 0,              med: 0, qty: 1, u: 1, w: 8  },
  { name: 'chest pain',           hosp: H.CARDIAC,         amb: A.ALS,          med: 1, qty: 1, u: 2, w: 10 },
  /* Two required specialties at once -- the hardest destination constraint. */
  { name: 'critical transfer',    hosp: H.ICU | H.CARDIAC, amb: A.ALS | A.VENT, med: 7, qty: 1, u: 3, w: 4  },
];
const CASE_TOTAL = CASES.reduce((a, c) => a + c.w, 0);
function pickCase() {
  let r = Math.random() * CASE_TOTAL;
  for (const c of CASES) { r -= c.w; if (r <= 0) return c; }
  return CASES[0];
}
/* Backlog of requests that could not be served on arrival. Ordered exactly
 * like the engine's min-max DEPQ: most urgent first, oldest first within a
 * tier, so a critical case jumps the queue ahead of minor ones already
 * waiting. Popped whenever a vehicle comes back into service. */
class Backlog {
  constructor() { this.a = []; }
  get size() { return this.a.length; }
  /* rank: lower is served sooner */
  static rank(urgency, seq) { return (3 - urgency) * 1e9 + seq; }
  push(item) {
    const a = this.a;
    a.push(item);
    let i = a.length - 1;
    while (i > 0) {
      const p = (i - 1) >> 1;
      if (a[p].rank <= a[i].rank) break;
      [a[p], a[i]] = [a[i], a[p]]; i = p;
    }
  }
  pop() {
    const a = this.a;
    const top = a[0], last = a.pop();
    if (a.length) {
      a[0] = last;
      let i = 0;
      for (;;) {
        const l = 2 * i + 1, r = l + 1;
        let m = i;
        if (l < a.length && a[l].rank < a[m].rank) m = l;
        if (r < a.length && a[r].rank < a[m].rank) m = r;
        if (m === i) break;
        [a[m], a[i]] = [a[i], a[m]]; i = m;
      }
    }
    return top;
  }
}

const sim = {
  running: false, rate: 0.5, acc: 0, timeScale: 60, nextId: 1,
  villages: [], missions: new Map(),
  dispatched: 0, failed: 0, slaMet: 0, requeued: 0, abandoned: 0,
  lat: [], settled: [], closedEdges: [],
  backlog: new Backlog(), seq: 0,
  clock: 10 * 3600000,          /* simulated time of day */
};

/* The road network is static, so it is pulled from the engine once and reused
   for every browser that connects. ~100k segments, paginated by node cursor. */
let roadsCache = null;
async function loadRoads() {
  if (roadsCache) return roadsCache;
  const out = { 0: [], 1: [], 2: [] };
  for (const cls of [0, 1, 2]) {
    let next = 0;
    while (next >= 0) {
      const r = await engine.send(`ROADS ${cls} ${next}`);
      if (!r.ok) break;
      /* NOT push(...seg): a page holds ~120k numbers and spreading that many
         arguments overflows the call stack. */
      const dst = out[cls], src = r.seg;
      for (let i = 0; i < src.length; i++) dst.push(src[i]);
      next = r.next;
    }
  }
  roadsCache = out;
  console.log(`roads cached: ${out[0].length / 4} highway, ${out[1].length / 4} arterial, ${out[2].length / 4} local`);
  return out;
}

async function loadStatics() {
  const b = await engine.send('BOUNDS');
  const h = await engine.send('HOSPITALS');
  const f = await engine.send('FLEET');
  const idx = Array.from({ length: 1200 }, (_, i) => `NODE ${i * 4}`);
  const nodes = await engine.batch(idx);
  sim.villages = nodes.filter((n) => n.ok).map((n) => ({ node: n.node, x: n.x, y: n.y }));
  return { bounds: b, hospitals: h.hospitals, fleet: f.fleet };
}

function pct(arr, q) {
  if (!arr.length) return 0;
  const a = [...arr].sort((x, y) => x - y);
  return a[Math.min(a.length - 1, Math.floor(a.length * q))];
}

function makeRequest() {
  const v = sim.villages[(Math.random() * sim.villages.length) | 0];
  if (!v) return null;
  const kase = pickCase();
  const sla = kase.u === 3 ? 8 * 60000 : 20 * 60000;
  return { v, kase, sla, seq: sim.seq++,
           rank: Backlog.rank(kase.u, sim.seq) };
}

/* Try to serve one request. Returns true if a vehicle was committed. */
async function attempt(req, fromBacklog) {
  const { v, kase, sla } = req;
  const r = await engine.send(
    `DISPATCH ${v.node} ${kase.hosp} ${kase.amb} ${kase.med} ${kase.qty} ` +
    `${kase.u} ${sla} 900000 1`);

  if (!r.ok) {
    /* An unserved request is not a dropped one: it goes into the urgency
       queue and is retried the moment capacity frees up. */
    if (!fromBacklog) {
      sim.failed++;
      sim.backlog.push(req);
      broadcast({ type: 'failed', reason: r.reason || r.error, incident: [v.x, v.y],
                  spec: kase.name, urgency: kase.u, queued: true,
                  considered: r.considered || 0 });
    } else {
      sim.backlog.push(req);          /* still blocked, keep its place */
    }
    return false;
  }

  sim.dispatched++;
  if (r.sla_met) sim.slaMet++;
  if (fromBacklog) sim.requeued++;
  sim.lat.push(r.latency_us);
  sim.settled.push(r.settled);
  if (sim.lat.length > 400) { sim.lat.shift(); sim.settled.shift(); }

  await engine.send(`COMMIT ${r.amb} ${r.hosp} ${kase.med} ${kase.qty}`);
  const id = sim.nextId++;
  sim.missions.set(id, { amb: r.amb, hosp: r.hosp });
  broadcast({
    type: 'dispatch', id, amb: r.amb, hosp: r.hosp,
    incident: [v.x, v.y], leg1: r.leg1 || [], leg2: r.leg2 || [],
    t_scene_ms: r.t_scene_ms, t_hosp_ms: r.t_hosp_ms, wait_ms: r.wait_ms,
    t_total_ms: r.t_total_ms, sla_met: r.sla_met,
    urgency: kase.u, spec: kase.name, med: MED[kase.med], qty: kase.qty,
    latency_us: r.latency_us, settled: r.settled, considered: r.considered,
    beds_free: r.beds_free, med_left: r.med_left,
    rejected: r.rejected || [], fromBacklog: !!fromBacklog,
  });

  /* Release the vehicle when its (time-compressed) run completes. */
  const wall = (r.t_scene_ms + r.t_hosp_ms) / sim.timeScale;
  setTimeout(async () => {
    await engine.send(`RELEASE ${r.amb} ${r.hosp}`);
    sim.missions.delete(id);
    broadcast({ type: 'release', id, amb: r.amb });
    drainBacklog();                   /* capacity freed -- serve the queue */
  }, Math.max(500, Math.min(wall, 60000)));
  return true;
}

async function spawnEmergency() {
  const req = makeRequest();
  if (req) await attempt(req, false);
}

/* Pop in urgency order and retry. Stops at the first request that still
   cannot be served, so one permanently-blocked case does not spin. */
let draining = false;
async function drainBacklog() {
  if (draining || !sim.backlog.size) return;
  draining = true;
  try {
    for (let i = 0; i < 8 && sim.backlog.size; i++) {
      const req = sim.backlog.pop();
      const served = await attempt(req, true);
      if (!served) break;
      broadcast({ type: 'log', level: 'ok',
        text: `backlog cleared: ${req.kase.name} (U${req.kase.u}) dispatched after waiting` });
    }
  } finally { draining = false; }
}

/* Accumulator, not a loop counter, so rates below one per second work --
   a human watching one case at a time needs ~0.5/s, not 3/s. */
setInterval(() => {
  if (!sim.running) return;
  sim.acc += sim.rate;
  while (sim.acc >= 1) { sim.acc -= 1; spawnEmergency(); }
}, 1000);

/* Advance the simulated clock so doctor shifts actually turn over during a
   demo: 60x compression puts a full 24h day in 24 real minutes. */
setInterval(async () => {
  if (!sim.running) return;
  sim.clock = (sim.clock + 2000 * sim.timeScale) % 86400000;
  await engine.send(`CLOCK ${sim.clock}`);
  drainBacklog();
}, 2000);

setInterval(async () => {
  if (!clients.size) return;
  const s = await engine.send('STATS');
  broadcast({
    type: 'telemetry',
    engine: s,
    running: sim.running, rate: sim.rate,
    dispatched: sim.dispatched, failed: sim.failed,
    backlog: sim.backlog.size, requeued: sim.requeued,
    active: sim.missions.size, clock: sim.clock,
    slaPct: sim.dispatched ? (100 * sim.slaMet) / sim.dispatched : 0,

    p50: pct(sim.lat, 0.5), p99: pct(sim.lat, 0.99),
    meanSettled: sim.settled.length
      ? sim.settled.reduce((a, b) => a + b, 0) / sim.settled.length : 0,
    closedRoads: sim.closedEdges.length,
  });
}, 500);

/* ---------------- client commands ---------------- */
async function onCommand(msg) {
  const m = JSON.parse(msg);
  if (m.type === 'run') sim.running = !!m.value;
  else if (m.type === 'rate') sim.rate = Math.max(0, Math.min(50, +m.value || 0));
  else if (m.type === 'oneshot') { await spawnEmergency(); }
  else if (m.type === 'surge') {
    const n = Math.max(1, Math.min(200, m.value | 0));
    for (let i = 0; i < n; i++) spawnEmergency();
    broadcast({ type: 'log', level: 'warn', text: `surge injected: ${n} simultaneous emergencies` });
  } else if (m.type === 'close') {
    const n = Math.max(1, Math.min(20000, m.value | 0));
    const cmds = [];
    for (let i = 0; i < n; i++) {
      const e = (Math.random() * 200100) | 0;
      sim.closedEdges.push(e);
      cmds.push(`CLOSE ${e}`);
    }
    const t = Date.now();
    await engine.batch(cmds);
    broadcast({ type: 'log', level: 'warn',
      text: `${n} roads closed in ${Date.now() - t} ms — hospital index now stale` });
  } else if (m.type === 'reopen') {
    const cmds = sim.closedEdges.splice(0).map((e) => `OPEN ${e}`);
    if (cmds.length) await engine.batch(cmds);
    broadcast({ type: 'log', level: 'ok', text: `${cmds.length} roads reopened` });
  } else if (m.type === 'restock') {
    const r = await engine.send('RESTOCK 999999 0 999999');
    for (let i = 1; i < 8; i++) await engine.send(`RESTOCK 999999 ${i} 999999`);
    broadcast({ type: 'log', level: 'ok', text: 'all medicine batches replenished to capacity' });
    drainBacklog();
  } else if (m.type === 'clock') {
    sim.clock = (m.value | 0) % 86400000;
    const r = await engine.send(`CLOCK ${sim.clock}`);
    broadcast({ type: 'log', level: 'warn',
      text: `clock set to ${String(Math.floor(sim.clock / 3600000)).padStart(2, '0')}:00 — `
          + `${r.docs_on_duty} doctors on duty across ${r.staffed_departments} departments` });
    drainBacklog();
  } else if (m.type === 'rebuild') {
    const r = await engine.send('REBUILD');
    broadcast({ type: 'log', level: 'ok',
      text: `hospital index rebuilt in ${r.took_ms} ms (generation ${r.generation})` });
  }
}

/* ---------------- http + upgrade ---------------- */
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
const server = http.createServer((req, res) => {
  const file = req.url === '/' ? 'index.html' : path.basename(req.url.split('?')[0]);
  const full = path.join(__dirname, file);
  fs.readFile(full, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(full)] || 'application/octet-stream' });
    res.end(data);
  });
});

server.on('upgrade', (req, sock) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) { sock.destroy(); return; }
  sock.write(
    'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n' +
    'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + wsAccept(key) + '\r\n\r\n');
  sock.setNoDelay(true);
  clients.add(sock);

  let buf = Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    const rest = wsRead(buf, (txt) => onCommand(txt).catch(() => {}));
    if (rest === null) { clients.delete(sock); sock.destroy(); return; }
    buf = rest;
  });
  sock.on('close', () => clients.delete(sock));
  sock.on('error', () => clients.delete(sock));

  loadStatics()
    .then((s) => {
      sock.write(wsFrame(JSON.stringify({ type: 'init', ...s })));
      return loadRoads();
    })
    .then((roads) => sock.write(wsFrame(JSON.stringify({ type: 'roads', roads }))))
    .catch((e) => console.error('static load failed:', e));
});

engine.ready().then(() => {
  server.listen(HTTP_PORT, () =>
    console.log(`bridge on http://127.0.0.1:${HTTP_PORT}  ->  engine ${ENGINE_HOST}:${ENGINE_PORT}`));
}).catch(() => {
  console.error(`cannot reach engine on ${ENGINE_HOST}:${ENGINE_PORT} — start it with ./server 9090`);
  process.exit(1);
});
