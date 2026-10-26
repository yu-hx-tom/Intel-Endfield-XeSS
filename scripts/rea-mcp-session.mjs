// Keep one real REA stdio MCP connection for a bounded sequence of analysis calls.
// Usage: node scripts/rea-mcp-session.mjs [plan.json | --interactive]
// A plan is {steps:[{name:"tool_name",arguments:{...}}]}; no target is opened by default.
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createInterface } from 'node:readline';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sdk = join(root, '.tools/rea/node_modules/@modelcontextprotocol/client/dist');
const { Client } = await import(pathToFileURL(join(sdk, 'index.mjs')));
const { StdioClientTransport } = await import(pathToFileURL(join(sdk, 'stdio.mjs')));
const runtime = JSON.parse(await readFile(join(root, '.tools/rea/runtime.json'), 'utf8'));
let steps = [];
const interactive = process.argv[2] === '--interactive';
if (process.argv[2] && !interactive) {
  const input = resolve(process.argv[2]);
  if ((await stat(input)).size > 65536) throw new Error('Plan exceeds 64 KiB.');
  const plan = JSON.parse(await readFile(input, 'utf8'));
  if (!Array.isArray(plan.steps) || plan.steps.length > 32) throw new Error('Expected up to 32 plan steps.');
  steps = plan.steps;
}
const output = join(root, 'out', `${new Date().toISOString().replace(/[:.]/g, '-')}-rea-mcp`);
await mkdir(output, { recursive: true });
await writeFile(join(output, 'plan.json'), JSON.stringify({ steps }, null, 2));
const transport = new StdioClientTransport({
  command: runtime.command, args: runtime.args, cwd: runtime.cwd ?? root,
  env: { ...process.env, ...runtime.env }, stderr: 'pipe',
});
let stderr = '';
transport.stderr?.on('data', (data) => { if (stderr.length < 65536) stderr += data.toString().slice(0, 65536 - stderr.length); });
const client = new Client({ name: 'endfield-research', version: '1.0.0' });
const summary = { output, server: null, steps: [], passed: false };
try {
  await client.connect(transport, { timeout: 60000 });
  summary.server = client.getServerVersion();
  const catalog = await client.listTools(undefined, { timeout: 60000 });
  await writeFile(join(output, 'tools.json'), JSON.stringify(catalog, null, 2));
  console.log(JSON.stringify({ event: 'connected', server: summary.server, toolCount: catalog.tools.length, output }));
  async function runStep(step) {
    const index = summary.steps.length;
    const definition = catalog.tools.find((tool) => tool.name === step.name);
    if (!definition) throw new Error(`Tool not found: ${step.name}`);
    await writeFile(join(output, `${String(index).padStart(2, '0')}-request.json`), JSON.stringify(step, null, 2));
    console.log(JSON.stringify({ event: 'calling', index, tool: step.name }));
    const result = await client.callTool({ name: step.name, arguments: step.arguments ?? {} }, { timeout: 1200000, toolDefinition: definition });
    const filename = `${String(index).padStart(2, '0')}-${step.name}.json`;
    await writeFile(join(output, filename), JSON.stringify(result, null, 2));
    const record = { index, tool: step.name, file: filename, isError: result.isError === true };
    summary.steps.push(record);
    console.log(JSON.stringify({ event: 'completed', ...record }));
    if (record.isError) throw new Error(`REA returned an error; inspect ${filename}`);
  }
  for (const step of steps) await runStep(step);
  if (interactive) {
    console.log(JSON.stringify({ event: 'ready', input: 'One {name,arguments} JSON request per line; {close:true} ends this session.' }));
    const lines = createInterface({ input: process.stdin, terminal: false });
    try {
      for await (const line of lines) {
        if (!line.trim()) continue;
        if (line.length > 65536) throw new Error('Request exceeds 64 KiB.');
        const request = JSON.parse(line);
        if (request.close === true) break;
        await runStep(request);
      }
    } finally { lines.close(); }
  }
  summary.passed = true;
} catch (error) {
  summary.error = String(error);
  console.error(summary.error);
  process.exitCode = 1;
} finally {
  await client.close();
  await writeFile(join(output, 'stderr.txt'), stderr);
  await writeFile(join(output, 'summary.json'), JSON.stringify(summary, null, 2));
}
