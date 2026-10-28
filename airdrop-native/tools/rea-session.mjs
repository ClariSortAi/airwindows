// Persistent, locally queued static analysis. No target execution or driver calls.
import { McpClient } from '../../scripts/mcp-client.mjs';
import { mkdir, readdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

const folder = path.resolve(process.argv[2]);
await mkdir(folder, { recursive: true });
const client = new McpClient(['-y', 'rea-agents@6.7.0', 'mcp']);
const allowed = new Set(['open_binary', 'close_binary', 'binary_session',
  'binary_overview', 'search_strings', 'search_procedures', 'procedure_pseudo_code',
  'analyze_function', 'get_xrefs', 'xrefs', 'get_procedure', 'procedure_assembly',
  'read_bytes', 'list_procedures', 'list_segments', 'trace_feature',
  'inspect_analysis_view', 'export_evidence_bundle']);
const done = new Set();
try {
  const init = await client.init();
  const list = await client.request('tools/list', {});
  await writeFile(path.join(folder, 'schemas.json'), JSON.stringify(list, null, 2));
  console.log(JSON.stringify({ready: true, server: init.serverInfo, pid: process.pid}));
  while (true) {
    const files = (await readdir(folder)).filter(n => /^\d+.*\.request\.json$/.test(n)).sort();
    let stop = false;
    for (const file of files) {
      if (done.has(file)) continue;
      done.add(file);
      const request = JSON.parse(await readFile(path.join(folder, file), 'utf8'));
      if (request.stop) { stop = true; break; }
      const resultFile = path.join(folder, file.replace('.request.json', '.result.json'));
      try {
        if (!allowed.has(request.tool)) throw new Error('Not an allowed static analysis tool');
        console.log(JSON.stringify({started: file, tool: request.tool}));
        const result = await client.callTool(request.tool, request.arguments ?? {}, 1800000);
        await writeFile(resultFile, JSON.stringify(result, null, 2), {flag: 'wx'});
        console.log(JSON.stringify({completed: file, error: result.isError ?? false}));
      } catch (error) {
        await writeFile(resultFile, JSON.stringify({error: String(error)}, null, 2), {flag: 'wx'});
        console.log(JSON.stringify({failed: file, error: String(error)}));
      }
    }
    if (stop) break;
    await new Promise(resolve => setTimeout(resolve, 250));
  }
} finally { client.close(); }
