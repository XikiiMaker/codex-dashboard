// Codex 状态桥：把本机 codex app-server 的配额与任务动态、以及公开重置雷达，
// 归一化成一份 JSON 供局域网内的 ESP32-P4 面板拉取。
//
// 面板侧只连这一个源、只走明文 HTTP，因此板子上不需要 TLS 与证书 bundle。
//
// 用法：node codex-bridge.js [--port 8787] [--privacy] [--token XXX] [--once]

'use strict';

const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const os = require('os');
const { spawn, execFileSync } = require('child_process');

// ---------------------------------------------------------------- 配置

const args = process.argv.slice(2);
function flag(name) {
  return args.includes(name);
}
function opt(name, fallback) {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}

const CONFIG = {
  port: Number(opt('--port', process.env.CODEX_BRIDGE_PORT || 8787)),
  privacy: flag('--privacy'),
  token: opt('--token', process.env.CODEX_BRIDGE_TOKEN || ''),
  once: flag('--once'),
  codexHome: process.env.CODEX_HOME || path.join(os.homedir(), '.codex'),
  /**
   * 媒体桥
   *
   * 面板上的音乐控制器够不着 PC 上的播放器：Windows 只通过 SMTC 暴露「正在播什么」
   * 和「反向控制播放」，那必须在 PC 本机调。活儿交给 media_server.py（Python + winsdk），
   * bridge 只做代理 —— 面板因此仍然只认 bridge 一个地址、一套 X-Panel-Token。
   *
   * --no-media 整块关掉；--no-media-spawn 则不自动拉起子进程，改为自己手动跑。
   */
  media: !flag('--no-media'),
  mediaPort: Number(opt('--media-port', process.env.CODEX_MEDIA_PORT || 8788)),
  mediaSpawn: !flag('--no-media-spawn'),
};

const QUOTA_INTERVAL_MS = 10 * 60 * 1000;
const ACTIVITY_INTERVAL_MS = 30 * 1000;
const RADAR_INTERVAL_MS = 60 * 60 * 1000;

// 归一化规则沿用上游 codex-zectrix-dashboard 的时效判定
/**
 * running 判定为「已死」的静默时长
 *
 * 不能照抄上游的 120 秒：`task_started` 在一轮开始时只写一次，之后无论跑多久都不再写，
 * 于是任何超过两分钟的运行中任务都会被当成残留丢掉——这正是面板上只看得见「本轮完成」
 * 的原因。上游敢用 120 秒是因为它还接了 hook-events.jsonl 那条持续刷新的通路。
 * 这里改成看 rollout 文件尾行时间戳（见 lastLineEpoch）：turn 跑着就一直有事件落盘。
 */
const RUNNING_STALE_SECONDS = 10 * 60;
const TASK_MAX_AGE_SECONDS = 24 * 60 * 60;
const NO_WATCH_FRESH_SECONDS = 2 * 60 * 60;
const CONFIRMATION_FRESH_SECONDS = 24 * 60 * 60;
/**
 * 面板上最多显示几条任务动态
 *
 * 上游的 3 条是为 400×300 墨水屏定的，1024×600 的下半屏放得下 5 行。
 * 改这个值必须同时改固件 codex_state.h 的 CODEX_MAX_TASKS，否则多出来的会被丢弃。
 */
const MAX_TASKS = 5;
const MAX_TITLE_CHARS = 48;
const RADAR_MAX_RESPONSE_BYTES = 64 * 1024;
const ROLLOUT_TAIL_BYTES = 256 * 1024;

const RADAR_URL = 'https://codex-resets.com/api/v1/status';

/** 媒体桥只在本机，慢就是 media_server.py 在查在线歌词或转封面，不是网络问题 */
const MEDIA_TIMEOUT_MS = 12000;
/** 封面是 128×128 的 RGB565 裸数据（32KB），歌词几 KB，留足余量但不给爆内存的机会 */
const MEDIA_MAX_RESPONSE_BYTES = 256 * 1024;
/** 控制命令的 body 只是 {"action":"play_pause","position_ms":1234}，1KB 绰绰有余 */
const CONTROL_MAX_BODY_BYTES = 1024;

const TASK_STATE_PRIORITY = { running: 0, failed: 1, interrupted: 2, turn_completed: 3 };

function log(...parts) {
  console.log(new Date().toISOString(), ...parts);
}

// ---------------------------------------------------------------- 定位 codex.exe

// Codex Desktop 把可执行文件放在带内容哈希的目录里，版本一变哈希就变，不能写死。
function discoverCodexExecutable() {
  if (process.env.CODEX_EXECUTABLE && fs.existsSync(process.env.CODEX_EXECUTABLE)) {
    return process.env.CODEX_EXECUTABLE;
  }
  const exeName = process.platform === 'win32' ? 'codex.exe' : 'codex';
  const roots = [];
  if (process.platform === 'win32') {
    const local = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
    roots.push(path.join(local, 'OpenAI', 'Codex', 'bin'));
  } else {
    roots.push('/usr/local/bin', path.join(os.homedir(), '.local', 'bin'));
  }
  const found = [];
  for (const root of roots) {
    let entries;
    try {
      entries = fs.readdirSync(root, { withFileTypes: true });
    } catch {
      continue;
    }
    const direct = path.join(root, exeName);
    if (fs.existsSync(direct)) found.push(direct);
    for (const entry of entries) {
      if (!entry.isDirectory()) continue;
      const candidate = path.join(root, entry.name, exeName);
      if (fs.existsSync(candidate)) found.push(candidate);
    }
  }
  if (found.length === 0) return null;
  // 同时装了多个版本时取最新落盘的那个
  found.sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);
  return found[0];
}

// ---------------------------------------------------------------- app-server 会话

class AppServerSession {
  constructor(program) {
    this.program = program;
    this.child = null;
    this.buf = '';
    this.pending = new Map();
    this.nextId = 2;
    this.ready = null;
  }

  async ensureReady() {
    if (this.ready) return this.ready;
    this.ready = this.#start().catch((err) => {
      this.ready = null;
      throw err;
    });
    return this.ready;
  }

  async #start() {
    log('app-server 启动', this.program);
    const child = spawn(this.program, ['app-server', '--stdio'], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
    this.child = child;
    this.buf = '';
    this.pending.clear();
    this.nextId = 2;

    child.stdout.on('data', (chunk) => this.#onData(chunk));
    child.stderr.on('data', (chunk) => {
      const text = chunk.toString('utf8').trim();
      if (text) log('app-server stderr:', text.slice(0, 300));
    });
    child.on('exit', (code, signal) => {
      log(`app-server 退出 code=${code} signal=${signal}`);
      this.#failAll(new Error('app-server exited'));
      this.child = null;
      this.ready = null;
    });
    child.on('error', (err) => {
      log('app-server 启动失败', err.message);
      this.#failAll(err);
      this.child = null;
      this.ready = null;
    });

    await this.#rawRequest(1, 'initialize', {
      clientInfo: { name: 'codex-panel-bridge', version: '0.1.0' },
    }, 20000);
    this.#write({ method: 'initialized' });
  }

  #onData(chunk) {
    this.buf += chunk.toString('utf8');
    let idx;
    while ((idx = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, idx).trim();
      this.buf = this.buf.slice(idx + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        continue;
      }
      if (msg.id !== undefined && this.pending.has(msg.id)) {
        const entry = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        clearTimeout(entry.timer);
        if (msg.error) entry.reject(new Error(`rpc error: ${JSON.stringify(msg.error).slice(0, 200)}`));
        else entry.resolve(msg.result);
      }
    }
  }

  #failAll(err) {
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
    this.pending.clear();
  }

  #write(obj) {
    if (!this.child || !this.child.stdin.writable) throw new Error('app-server not writable');
    this.child.stdin.write(JSON.stringify(obj) + '\n');
  }

  #rawRequest(id, method, params, timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`rpc timeout: ${method}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      try {
        this.#write({ id, method, params });
      } catch (err) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(err);
      }
    });
  }

  async request(method, params, timeoutMs = 15000) {
    await this.ensureReady();
    return this.#rawRequest(this.nextId++, method, params, timeoutMs);
  }

  stop() {
    const child = this.child;
    if (!child) return;
    this.child = null;
    this.ready = null;
    try {
      child.stdin.end();
    } catch {}
    // Windows 上没有进程组信号，app-server 会派生子进程，必须整棵树杀掉
    if (process.platform === 'win32') {
      try {
        execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
      } catch {}
    } else {
      try {
        child.kill('SIGTERM');
      } catch {}
    }
  }
}

// ---------------------------------------------------------------- 配额

function formatWindowName(minutes) {
  if (!Number.isFinite(minutes) || minutes <= 0) return '额度';
  if (minutes % 1440 === 0) return `${minutes / 1440} 天`;
  if (minutes % 60 === 0) return `${minutes / 60} 小时`;
  return `${minutes} 分钟`;
}

function normalizeWindow(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const used = raw.usedPercent;
  const mins = raw.windowDurationMins;
  const resets = raw.resetsAt;
  if (typeof used !== 'number' || used < 0 || used > 100) return null;
  if (typeof resets !== 'number') return null;
  return {
    name: formatWindowName(mins),
    window_minutes: typeof mins === 'number' ? mins : 0,
    used_percent: Math.round(used),
    remaining_percent: 100 - Math.round(used),
    resets_at: Math.round(resets),
  };
}

function parseQuota(result) {
  const limits = result && result.rateLimits;
  if (!limits) throw new Error('rateLimits 缺失');
  const windows = [normalizeWindow(limits.primary), normalizeWindow(limits.secondary)].filter(Boolean);
  if (windows.length === 0) throw new Error('没有可用的配额窗口');
  const credits = result.rateLimitResetCredits;
  return {
    windows,
    reset_credits: credits && typeof credits.availableCount === 'number' ? credits.availableCount : 0,
    plan_type: typeof limits.planType === 'string' ? limits.planType : '',
    limit_name: typeof limits.limitName === 'string' ? limits.limitName : '',
  };
}

// ---------------------------------------------------------------- 任务动态

// thread/list 只给标题与更新时间，执行状态得从会话 rollout 里读。
async function readThreadIndex(session) {
  const result = await session.request('thread/list', {
    cursor: null,
    limit: 30,
    sortKey: 'updated_at',
    sortDirection: 'desc',
    archived: false,
    sourceKinds: [
      'cli', 'vscode', 'exec', 'appServer', 'subAgent', 'subAgentReview',
      'subAgentCompact', 'subAgentThreadSpawn', 'subAgentOther', 'unknown',
    ],
    useStateDbOnly: true,
  });
  const data = result && Array.isArray(result.data) ? result.data : [];
  const index = new Map();
  for (const item of data) {
    if (!item || typeof item.id !== 'string') continue;
    // 子代理会话（评估、compact、review）不是用户眼里的任务，它们也没有 name
    if (item.parentThreadId) continue;
    const title = pickTitle(item);
    if (!title) continue;
    const entry = {
      id: item.id,
      title,
      updated_at: typeof item.updatedAt === 'number' ? item.updatedAt : 0,
    };
    index.set(item.id, entry);
    // rollout 文件名带的是 sessionId，与 thread id 不总是同一个
    if (typeof item.sessionId === 'string' && item.sessionId) index.set(item.sessionId, entry);
  }
  return index;
}

// 只认用户可见的会话名。preview 是原始 prompt 的开头，可能有几万字，不能当标题。
function pickTitle(item) {
  const value = item.name;
  if (typeof value !== 'string') return '';
  const cleaned = value.trim().replace(/\s+/g, ' ');
  if (!cleaned) return '';
  return cleaned.length > MAX_TITLE_CHARS ? `${cleaned.slice(0, MAX_TITLE_CHARS)}…` : cleaned;
}

/**
 * 列出全部 rollout 文件，**不按 mtime 过滤**
 *
 * 别再加 mtime 时效判断：Codex 正在写的 rollout 文件，Windows 不刷新它的 mtime
 * （实测文件内最后一条事件是 1 分钟前，stat 报的 mtime 却是 2 小时前）。按 mtime
 * 筛就会把唯一真正在跑的会话筛掉，这是「任务动态只显示已完成」的第一层原因。
 * 唯一可信的时间来自文件内的 timestamp 字段。
 *
 * 不做过滤也不会失控：调用方先用 thread/list（limit 30）的索引挡掉绝大多数文件，
 * 真正被读尾部的不超过 30 个。
 */
function listRolloutFiles() {
  const root = path.join(CONFIG.codexHome, 'sessions');
  const files = [];
  const walk = (dir, depth) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        if (depth < 3) walk(full, depth + 1);
      } else if (entry.isFile() && entry.name.startsWith('rollout-') && entry.name.endsWith('.jsonl')) {
        let stat;
        try {
          stat = fs.statSync(full);
        } catch {
          continue;
        }
        files.push({ file: full, mtime: stat.mtimeMs, size: stat.size });
      }
    }
  };
  walk(root, 0);
  // mtime 只用来排序，不用来判定时效
  files.sort((a, b) => b.mtime - a.mtime);
  return files;
}

// rollout 会长到几十 MB，只读尾部并丢掉被切断的首行
function readTailLines(file, size) {
  const start = Math.max(0, size - ROLLOUT_TAIL_BYTES);
  let fd;
  try {
    fd = fs.openSync(file, 'r');
  } catch {
    return [];
  }
  try {
    const length = size - start;
    if (length <= 0) return [];
    const buf = Buffer.allocUnsafe(length);
    fs.readSync(fd, buf, 0, length, start);
    const text = buf.toString('utf8');
    const lines = text.split('\n');
    if (start > 0) lines.shift();
    return lines;
  } catch {
    return [];
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * 取该会话最后一次生命周期事件，决定它当前处于什么状态
 *
 * 返回的 heartbeat 是文件尾行的时间戳，代表「这个会话最近一次往盘上写东西是什么时候」。
 * running 的存活判定必须用它而不是 at：`task_started` 只在开头写一次。
 */
function readSessionActivity(file, size, nowSeconds) {
  const lines = readTailLines(file, size);
  const heartbeat = lastLineEpoch(lines);
  for (let i = lines.length - 1; i >= 0; i -= 1) {
    const line = lines[i].trim();
    if (!line) continue;
    if (!line.includes('task_started') && !line.includes('task_complete') && !line.includes('turn_aborted')) {
      continue;
    }
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      continue;
    }
    const payload = msg && msg.payload;
    const kind = payload && payload.type;
    if (kind !== 'task_started' && kind !== 'task_complete' && kind !== 'turn_aborted') continue;
    if (kind === 'task_started') {
      const at = eventEpoch(msg, payload, ['started_at']);
      return at ? { state: 'running', at, heartbeat } : null;
    }
    if (kind === 'turn_aborted') {
      const at = eventEpoch(msg, payload, ['ended_at', 'started_at']);
      return at ? { state: 'interrupted', at, heartbeat } : null;
    }
    // task_complete 的 started_at 是这一轮的**开始**时间，结束时间在 completed_at
    const at = eventEpoch(msg, payload, ['completed_at', 'ended_at', 'started_at']);
    return at ? { state: payload.is_error === true ? 'failed' : 'turn_completed', at, heartbeat } : null;
  }
  /*
   * 尾窗里一条生命周期事件都没有：一轮写超过 ROLLOUT_TAIL_BYTES 时，task_started 会
   * 被挤到窗口外。而 task_complete 是一轮的最后一条，若它存在就必然落在尾部——所以
   * 「窗内无事件 + 文件仍在被追加」只能是这一轮还在跑。
   */
  if (heartbeat && nowSeconds - heartbeat <= RUNNING_STALE_SECONDS) {
    return { state: 'running', at: heartbeat, heartbeat };
  }
  return null;
}

/**
 * 尾行时间戳＝会话心跳
 *
 * 只看最后几行，且用正则而不是 JSON.parse：单行可能有几万字（完整 prompt / 工具输出），
 * 每轮轮询都全量解析太贵。
 */
function lastLineEpoch(lines) {
  for (let i = lines.length - 1, tries = 0; i >= 0 && tries < 8; i -= 1) {
    const line = lines[i].trim();
    if (!line) continue;
    tries += 1;
    const match = line.match(/"timestamp"\s*:\s*"([^"]+)"/);
    if (!match) continue;
    const ms = Date.parse(match[1]);
    if (Number.isFinite(ms)) return Math.floor(ms / 1000);
  }
  return 0;
}

function eventEpoch(msg, payload, preferred) {
  for (const key of preferred) {
    if (payload && typeof payload[key] === 'number') return payload[key];
  }
  if (msg && typeof msg.timestamp === 'string') {
    const ms = Date.parse(msg.timestamp);
    if (Number.isFinite(ms)) return Math.floor(ms / 1000);
  }
  return 0;
}

function sessionIdFromPath(file) {
  const base = path.basename(file);
  const match = base.match(/rollout-.*?-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\.jsonl$/i);
  return match ? match[1] : '';
}

async function collectTasks(session, nowSeconds) {
  const index = await readThreadIndex(session);
  const cutoff = nowSeconds - TASK_MAX_AGE_SECONDS;
  const candidates = [];
  for (const entry of listRolloutFiles()) {
    const id = sessionIdFromPath(entry.file);
    const meta = id ? index.get(id) : null;
    // 索引里没有它，说明这是被过滤掉的子代理会话或未命名会话
    if (!meta) continue;
    const activity = readSessionActivity(entry.file, entry.size, nowSeconds);
    if (!activity) continue;
    if (activity.state === 'running') {
      // 进程被强杀时不会补写结束事件，只有还在往盘上写的才算真的在跑
      const beat = activity.heartbeat || activity.at;
      if (nowSeconds - beat > RUNNING_STALE_SECONDS) continue;
    } else if (activity.at < cutoff) {
      continue;
    }
    candidates.push({
      title: meta.title,
      state: activity.state,
      activity_at: activity.at,
    });
  }
  candidates.sort((a, b) => {
    const pa = TASK_STATE_PRIORITY[a.state] ?? 9;
    const pb = TASK_STATE_PRIORITY[b.state] ?? 9;
    if (pa !== pb) return pa - pb;
    return b.activity_at - a.activity_at;
  });
  const visible = candidates.slice(0, MAX_TASKS).map((task) => ({
    title: CONFIG.privacy ? '隐私任务' : task.title,
    state: task.state,
    activity_at: task.activity_at,
  }));
  return { tasks: visible, hidden_task_count: Math.max(0, candidates.length - visible.length) };
}

// ---------------------------------------------------------------- 重置雷达

let radarEtag = '';
let radarRaw = null;

function fetchRadar() {
  return new Promise((resolve, reject) => {
    const headers = { 'User-Agent': 'codex-panel-bridge/0.1.0', Accept: 'application/json' };
    if (radarEtag) headers['If-None-Match'] = radarEtag;
    const req = https.get(RADAR_URL, { headers, timeout: 8000 }, (res) => {
      if (res.statusCode === 304) {
        res.resume();
        resolve({ notModified: true });
        return;
      }
      if (res.statusCode !== 200) {
        res.resume();
        reject(new Error(`radar http ${res.statusCode}`));
        return;
      }
      let size = 0;
      const chunks = [];
      res.on('data', (chunk) => {
        size += chunk.length;
        if (size > RADAR_MAX_RESPONSE_BYTES) {
          req.destroy();
          reject(new Error('radar response too large'));
          return;
        }
        chunks.push(chunk);
      });
      res.on('end', () => {
        try {
          const parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
          if (res.headers.etag) radarEtag = res.headers.etag;
          resolve({ body: parsed });
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on('timeout', () => req.destroy(new Error('radar timeout')));
    req.on('error', reject);
  });
}

function rfc3339ToEpoch(value) {
  if (typeof value !== 'string') return 0;
  const ms = Date.parse(value);
  return Number.isFinite(ms) ? Math.floor(ms / 1000) : 0;
}

function parseRadar(body) {
  const data = body && body.data;
  // 两个键必须都出现，否则说明拿到的不是这个接口的响应
  if (!data || !('latest_reset' in data) || !('active_watch' in data)) {
    throw new Error('radar envelope 无效');
  }
  const rawWatch = data.active_watch;
  let watch = null;
  if (rawWatch && typeof rawWatch === 'object') {
    const level = rawWatch.level;
    const observed = rfc3339ToEpoch(rawWatch.observed_at);
    const expires = rfc3339ToEpoch(rawWatch.expires_at);
    const window = typeof rawWatch.forecast_window === 'string' ? rawWatch.forecast_window.trim() : '';
    const chance = rawWatch.reset_chance_percent;
    const chanceOk = chance === null || chance === undefined || (typeof chance === 'number' && chance >= 0 && chance <= 100);
    // 上游没公开 elevated/strong 之外等级的含义，不认识就整条丢弃
    if ((level === 'elevated' || level === 'strong') && window && observed && expires > observed && chanceOk) {
      watch = {
        level,
        reset_chance_percent: typeof chance === 'number' ? Math.round(chance) : null,
        forecast_window: window,
        observed_at: observed,
        expires_at: expires,
      };
    }
  }
  const rawReset = data.latest_reset;
  let confirmation = null;
  if (rawReset && typeof rawReset === 'object') {
    const kind = rawReset.reset_type === 'banked' ? 'reset_credit' : rawReset.reset_type === 'regular' ? 'regular' : '';
    const announced = rfc3339ToEpoch(rawReset.announced_at);
    if (kind && announced) confirmation = { kind, announced_at: announced };
  }
  return { watch, confirmation, observed_at: Math.floor(Date.now() / 1000) };
}

function radarDisplayState(nowSeconds) {
  if (!radarRaw) return { state: 'unavailable' };
  const { watch, confirmation, observed_at: observedAt } = radarRaw;
  if (watch && nowSeconds < watch.expires_at) {
    // 预测比确认事件更新时才让它盖住确认状态
    if (!confirmation || watch.observed_at >= confirmation.announced_at) {
      return {
        state: 'active_watch',
        level: watch.level,
        reset_chance_percent: watch.reset_chance_percent,
        forecast_window: watch.forecast_window,
      };
    }
  }
  if (confirmation && nowSeconds - confirmation.announced_at <= CONFIRMATION_FRESH_SECONDS) {
    return { state: 'confirmed', kind: confirmation.kind, announced_at: confirmation.announced_at };
  }
  if (!watch && !confirmation && nowSeconds - observedAt <= NO_WATCH_FRESH_SECONDS) {
    return { state: 'no_active_watch' };
  }
  if (!watch && nowSeconds - observedAt <= NO_WATCH_FRESH_SECONDS) {
    return { state: 'no_active_watch' };
  }
  return { state: 'unavailable' };
}

// ---------------------------------------------------------------- 状态汇总

const state = {
  rev: 0,
  quota: null,
  quotaObservedAt: 0,
  tasks: [],
  hiddenTaskCount: 0,
  activityObservedAt: 0,
  activityAvailable: false,
  radarObservedAt: 0,
};

let cachedPayload = '';
let cachedEtag = '';

function buildPayload() {
  const now = Math.floor(Date.now() / 1000);
  const quotaAge = state.quotaObservedAt ? now - state.quotaObservedAt : Infinity;
  const activityAge = state.activityObservedAt ? now - state.activityObservedAt : Infinity;
  const radar = radarDisplayState(now);
  // 面板没有可靠的绝对时间，所有时间点都在这里换算成相对秒数
  if (radar.state === 'confirmed') {
    radar.announced_ago_seconds = Math.max(0, now - radar.announced_at);
  }
  return {
    rev: state.rev,
    generated_at: now,
    privacy: CONFIG.privacy,
    quota: state.quota
      ? {
          ...state.quota,
          windows: state.quota.windows.map((w) => ({
            ...w,
            resets_in_seconds: Math.max(0, w.resets_at - now),
          })),
          stale: quotaAge > (QUOTA_INTERVAL_MS / 1000) * 2,
        }
      : null,
    radar,
    radar_stale: state.radarObservedAt ? now - state.radarObservedAt > (RADAR_INTERVAL_MS / 1000) * 2 : true,
    tasks: state.tasks.map((task) => ({
      ...task,
      activity_ago_seconds: Math.max(0, now - task.activity_at),
    })),
    hidden_task_count: state.hiddenTaskCount,
    task_activity_available: state.activityAvailable,
    task_activity_stale: activityAge > (ACTIVITY_INTERVAL_MS / 1000) * 4,
  };
}

// rev 表示面板真的会看到不同内容。随时钟漂移的派生字段每轮都在变，不能算进比较。
function stripVolatile(payload) {
  const copy = {
    ...payload,
    rev: 0,
    generated_at: 0,
    radar: { ...payload.radar, announced_ago_seconds: 0 },
    tasks: payload.tasks.map((task) => ({ ...task, activity_ago_seconds: 0 })),
  };
  if (copy.quota) {
    copy.quota = {
      ...copy.quota,
      windows: copy.quota.windows.map((w) => ({ ...w, resets_in_seconds: 0 })),
    };
  }
  return JSON.stringify(copy);
}

function refreshPayload() {
  const payload = buildPayload();
  const text = JSON.stringify(payload);
  const comparable = stripVolatile(payload);
  if (comparable !== refreshPayload.lastComparable) {
    refreshPayload.lastComparable = comparable;
    state.rev += 1;
    payload.rev = state.rev;
    cachedPayload = JSON.stringify(payload);
    cachedEtag = `"r${state.rev}"`;
  } else {
    cachedPayload = text;
  }
  return cachedPayload;
}
refreshPayload.lastComparable = null;

// ---------------------------------------------------------------- 观察循环

const session = new AppServerSession(null);

async function observeQuota() {
  try {
    const result = await session.request('account/rateLimits/read', null, 20000);
    state.quota = parseQuota(result);
    state.quotaObservedAt = Math.floor(Date.now() / 1000);
    log('配额已更新', state.quota.windows.map((w) => `${w.name} 剩余${w.remaining_percent}%`).join(' / '));
  } catch (err) {
    log('配额读取失败:', err.message);
    session.stop();
  }
  refreshPayload();
}

async function observeActivity() {
  try {
    const now = Math.floor(Date.now() / 1000);
    const { tasks, hidden_task_count: hidden } = await collectTasks(session, now);
    state.tasks = tasks;
    state.hiddenTaskCount = hidden;
    state.activityAvailable = true;
    state.activityObservedAt = now;
  } catch (err) {
    log('任务动态读取失败:', err.message);
    state.activityAvailable = false;
    session.stop();
  }
  refreshPayload();
}

async function observeRadar() {
  try {
    const result = await fetchRadar();
    if (!result.notModified) radarRaw = parseRadar(result.body);
    state.radarObservedAt = Math.floor(Date.now() / 1000);
    log('雷达已更新', JSON.stringify(radarDisplayState(state.radarObservedAt)));
  } catch (err) {
    log('雷达读取失败:', err.message);
  }
  refreshPayload();
}

// ---------------------------------------------------------------- 媒体桥代理

let mediaChild = null;

/**
 * 转发给本机的 media_server.py
 *
 * bridge 不解析媒体内容，原样透传：媒体字段的语义都在 media_server.py 那边，
 * 这里再解一遍只会多一处需要同步改的地方。返回 null 表示子进程没起、崩了或超时，
 * 由调用方降级。
 */
function proxyMedia(method, subPath, body, forwardHeaders) {
  return new Promise((resolve) => {
    const headers = { ...(forwardHeaders || {}) };
    if (body && body.length) headers['Content-Length'] = body.length;
    const req = http.request(
      {
        host: '127.0.0.1',
        port: CONFIG.mediaPort,
        path: subPath,
        method,
        headers,
        timeout: MEDIA_TIMEOUT_MS,
      },
      (res) => {
        const chunks = [];
        let size = 0;
        let aborted = false;
        res.on('data', (chunk) => {
          size += chunk.length;
          if (size > MEDIA_MAX_RESPONSE_BYTES) {
            aborted = true;
            req.destroy();
            return;
          }
          chunks.push(chunk);
        });
        res.on('end', () => resolve(aborted ? null : {
          status: res.statusCode,
          headers: res.headers,
          body: Buffer.concat(chunks),
        }));
      },
    );
    req.on('timeout', () => req.destroy());
    req.on('error', () => resolve(null));
    if (body && body.length) req.write(body);
    req.end();
  });
}

/** 超限返回 null，与「空 body」区分开 —— 空 body 是合法的，超限要拒 */
function readRequestBody(req, limit) {
  return new Promise((resolve) => {
    const chunks = [];
    let size = 0;
    let overflow = false;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > limit) {
        overflow = true;
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(overflow ? null : Buffer.concat(chunks)));
    req.on('error', () => resolve(null));
  });
}

function sendJson(res, status, payload) {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': body.length,
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

const MEDIA_ROUTES = {
  '/api/media': '/media',
  '/api/media/cover': '/cover',
  '/api/media/lyrics': '/lyrics',
  '/api/media/control': '/control',
};

async function handleMedia(req, res, url) {
  if (!CONFIG.media) {
    sendJson(res, 200, { available: false, media_bridge: false, error: '媒体桥已用 --no-media 关闭' });
    return;
  }
  const subPath = MEDIA_ROUTES[url.pathname];
  if (!subPath) {
    sendJson(res, 404, { ok: false, error: 'not found' });
    return;
  }

  let body = null;
  if (req.method === 'POST') {
    if (subPath !== '/control') {
      sendJson(res, 405, { ok: false, error: `${url.pathname} 不接受 POST` });
      return;
    }
    body = await readRequestBody(req, CONTROL_MAX_BODY_BYTES);
    if (body === null) {
      sendJson(res, 413, { ok: false, error: 'body 过大' });
      return;
    }
  } else if (subPath === '/control') {
    sendJson(res, 405, { ok: false, error: '/api/media/control 只接受 POST' });
    return;
  }

  // 条件请求要透传：面板只在切歌时才值得重传那 32KB 封面
  const forward = {};
  if (req.headers['if-none-match']) forward['If-None-Match'] = req.headers['if-none-match'];

  const result = await proxyMedia(req.method, subPath, body, forward);
  if (result === null) {
    /*
     * media_bridge:false 和 available:false 是两件不同的事：前者是 PC 上的媒体服务
     * 根本没跑，后者是服务在跑但没有播放器在放歌。面板要能分别提示，不能都显示成
     * 「没有音乐」—— 前者是用户该去起服务，后者是正常的空闲状态。
     *
     * 这里回 200 而不是 5xx：媒体是附加功能，用错误码会让面板把它当成链路故障，
     * 而额度和任务动态其实一切正常。
     */
    sendJson(res, 200, {
      available: false,
      media_bridge: false,
      error: '媒体服务没有响应',
      hint: `确认 media_server.py 是否在 127.0.0.1:${CONFIG.mediaPort} 上运行`,
    });
    return;
  }

  const headers = {
    'Content-Type': result.headers['content-type'] || 'application/octet-stream',
    'Content-Length': result.body.length,
    'Cache-Control': 'no-store',
  };
  if (result.headers.etag) headers.ETag = result.headers.etag;
  for (const key of ['x-cover-width', 'x-cover-height', 'x-cover-format']) {
    if (result.headers[key]) headers[key] = result.headers[key];
  }
  res.writeHead(result.status, headers);
  res.end(result.body);
}

/**
 * 拉起 media_server.py
 *
 * 找不到 venv 或脚本就不拉，只在日志里写清楚该怎么手动起 —— 媒体控制器是附加功能，
 * 不该因为它没装好就让整个 bridge 起不来，额度和任务动态还得照常下发。
 */
function startMediaChild() {
  if (!CONFIG.media || !CONFIG.mediaSpawn) return;
  const python = path.join(__dirname, '.venv-media', 'Scripts', 'python.exe');
  const script = path.join(__dirname, 'media_server.py');
  if (!fs.existsSync(script)) {
    log('媒体桥跳过：找不到 media_server.py');
    return;
  }
  if (!fs.existsSync(python)) {
    log('媒体桥跳过：找不到 .venv-media，先在 bridge 目录跑一次');
    log('  py -m venv .venv-media');
    log('  .venv-media\\Scripts\\python.exe -m pip install -r requirements-media.txt');
    log(`之后可用 --no-media-spawn 自己起：.venv-media\\Scripts\\python.exe media_server.py --port ${CONFIG.mediaPort}`);
    return;
  }
  const child = spawn(python, [script, '--port', String(CONFIG.mediaPort)], {
    cwd: __dirname,
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  // 子进程日志加前缀直接并到 bridge 的输出里，排查时不用开两个窗口对着看
  child.stdout.on('data', (text) => process.stdout.write(`[media] ${text}`));
  child.stderr.on('data', (text) => process.stdout.write(`[media] ${text}`));
  child.on('exit', (code, signal) => {
    log(`媒体桥子进程退出 code=${code} signal=${signal}，面板会转为显示「媒体服务没有响应」`);
    if (mediaChild === child) mediaChild = null;
  });
  child.on('error', (err) => log('媒体桥子进程启动失败:', err.message));
  mediaChild = child;
  log(`媒体桥已拉起，监听 127.0.0.1:${CONFIG.mediaPort}`);
}

function stopMediaChild() {
  if (mediaChild && !mediaChild.killed) {
    mediaChild.kill();
    mediaChild = null;
  }
}

// ---------------------------------------------------------------- HTTP 服务

function lanAddresses() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const item of list || []) {
      if (item.family === 'IPv4' && !item.internal) out.push(item.address);
    }
  }
  return out;
}

function startServer() {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    if (CONFIG.token) {
      const provided = req.headers['x-panel-token'] || url.searchParams.get('token') || '';
      if (provided !== CONFIG.token) {
        res.writeHead(401, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('unauthorized');
        return;
      }
    }
    if (url.pathname === '/health') {
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({
        ok: true,
        rev: state.rev,
        quota: Boolean(state.quota),
        media: CONFIG.media,
        media_child: Boolean(mediaChild),
      }));
      return;
    }
    if (url.pathname.startsWith('/api/media')) {
      /*
       * handleMedia 是 async，rejection 必须在这里兜住：unhandled rejection 会让
       * 整个 bridge 进程退出，额度和任务动态跟着一起没 —— 媒体是附加功能，
       * 它出问题不该连累主链路。
       */
      handleMedia(req, res, url).catch((err) => {
        log('媒体代理异常:', err && err.message);
        if (!res.headersSent) sendJson(res, 200, { available: false, media_bridge: false, error: '媒体代理异常' });
        else res.end();
      });
      return;
    }
    if (url.pathname !== '/api/dashboard') {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('not found');
      return;
    }
    const body = cachedPayload || refreshPayload();
    if (cachedEtag && req.headers['if-none-match'] === cachedEtag) {
      res.writeHead(304, { ETag: cachedEtag });
      res.end();
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'application/json; charset=utf-8',
      'Content-Length': Buffer.byteLength(body),
      'Cache-Control': 'no-cache',
      ETag: cachedEtag,
    });
    res.end(body);
  });
  server.listen(CONFIG.port, '0.0.0.0', () => {
    log(`HTTP 就绪 :${CONFIG.port}`);
    for (const addr of lanAddresses()) log(`  面板可用地址 http://${addr}:${CONFIG.port}/api/dashboard`);
    if (CONFIG.media) {
      log('  媒体端点 /api/media /api/media/cover /api/media/lyrics /api/media/control(POST)');
    } else {
      log('  媒体桥已用 --no-media 关闭，面板音乐控制器会显示「媒体服务未启用」');
    }
  });
  return server;
}

// ---------------------------------------------------------------- 入口

async function main() {
  const program = discoverCodexExecutable();
  if (!program) {
    console.error('找不到 codex 可执行文件，用 CODEX_EXECUTABLE 指定绝对路径');
    process.exit(1);
  }
  session.program = program;
  log('codex:', program);
  log('CODEX_HOME:', CONFIG.codexHome);
  if (CONFIG.privacy) log('隐私模式已开启，任务标题不外发');

  await observeQuota();
  await observeActivity();
  await observeRadar();

  if (CONFIG.once) {
    console.log(JSON.stringify(JSON.parse(refreshPayload()), null, 2));
    session.stop();
    return;
  }

  // 媒体桥先起：等面板连上时 /api/media 就该是可用状态，而不是头几十秒一直报「没有响应」
  startMediaChild();
  startServer();
  const timers = [
    setInterval(observeQuota, QUOTA_INTERVAL_MS),
    setInterval(observeActivity, ACTIVITY_INTERVAL_MS),
    setInterval(observeRadar, RADAR_INTERVAL_MS),
  ];

  const shutdown = () => {
    log('退出中');
    timers.forEach(clearInterval);
    // 不收掉子进程的话，bridge 退了 Python 还占着 8788，下次启动会撞端口
    stopMediaChild();
    session.stop();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

main().catch((err) => {
  console.error('bridge 启动失败:', err);
  session.stop();
  process.exit(1);
});
