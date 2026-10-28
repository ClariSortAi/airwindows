// Minimal MCP stdio client for REA's server (newline-delimited JSON-RPC). Used only because REA's CLI cannot
// write session evidence bundles; the MCP server is the one supported source of bundles.
import { spawn } from 'node:child_process';

export class McpClient {
  constructor(args = ['-y', 'rea-agents@latest', 'mcp'], { env = {} } = {}) {
    this.child = spawn('npx.cmd', args, { shell: true, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true, env: { ...process.env, ...env } });
    this.nextId = 1;
    this.pending = new Map();
    this.buf = '';
    this.stderr = '';
    this.child.stdout.on('data', (d) => this.onData(d));
    this.child.stderr.on('data', (d) => { this.stderr += d.toString(); });
    this.exited = new Promise((resolve) => this.child.on('exit', (code) => {
      for (const { reject } of this.pending.values()) reject(new Error(`MCP server exited (${code}): ${this.stderr.slice(-400)}`));
      resolve(code);
    }));
  }
  onData(d) {
    this.buf += d.toString();
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch (err) {
        console.error('mcp: non-JSON line from server:', line.slice(0, 120), err.message);
        continue;
      }
      if (msg.id !== undefined && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(JSON.stringify(msg.error)));
        else resolve(msg.result);
      }
    }
  }
  request(method, params, timeoutMs = 120000) {
    const id = this.nextId++;
    this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => { this.pending.delete(id); reject(new Error(`timeout waiting for ${method}`)); }, timeoutMs);
      this.pending.set(id, { resolve: (v) => { clearTimeout(t); resolve(v); }, reject: (e) => { clearTimeout(t); reject(e); } });
    });
  }
  notify(method, params) {
    this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
  }
  async init() {
    const res = await this.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'rea-run-script', version: '1.0.0' } }, 90000);
    this.notify('notifications/initialized', {});
    return res;
  }
  callTool(name, args, timeoutMs = 300000) {
    return this.request('tools/call', { name, arguments: args }, timeoutMs);
  }
  close() {
    this.child.stdin.end();
    spawn('taskkill', ['/PID', String(this.child.pid), '/T', '/F'], { windowsHide: true });
  }
}

if (process.argv[1].endsWith('mcp-client.mjs') && process.argv[2] === 'schemas') {
  const c = new McpClient();
  try {
    const init = await c.init();
    console.log('server:', JSON.stringify(init.serverInfo), 'protocol:', init.protocolVersion);
    const list = await c.request('tools/list', {});
    const want = new Set(['inspect_web_page', 'export_evidence_bundle', 'list_browser_targets', 'compare_bundles']);
    console.log('tool count:', list.tools.length);
    for (const t of list.tools.filter((t) => want.has(t.name))) {
      console.log('\n=== ' + t.name + ' ===\n' + t.description.slice(0, 400) + '\n' + JSON.stringify(t.inputSchema.properties ?? {}, null, 0).slice(0, 1800) + '\nrequired: ' + JSON.stringify(t.inputSchema.required));
    }
  } catch (err) {
    console.error('schemas failed:', err.message);
    process.exitCode = 1;
  } finally {
    c.close();
  }
}
