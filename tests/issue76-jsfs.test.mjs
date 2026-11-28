import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const source = await readFile(new URL('../sys/src/9/wasm32/jsfs.js', import.meta.url), 'utf8');
const { JsFs, CHUNK } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
const enc = new TextEncoder(), dec = new TextDecoder();
test('a pending native wait does not prevent another request completing', async () => {
  const fs = new JsFs(), sent = [];
  fs.attach(b => sent.push(dec.decode(b)));
  const waiting = fs.wait('7', 0, CHUNK + 1);
  const ping = fs.ping();
  assert.equal(sent[0], '1 wait 7 0 ' + CHUNK + '\n');
  assert.equal(sent[1], '2 ping\n');
  fs.receive(enc.encode('2 ok\n'));
  await ping;
  assert.equal(fs.pending.size, 1);
  fs.receive(enc.encode('1 ok 6\ndirty\n'));
  assert.equal(dec.decode(await waiting), 'dirty\n');
  assert.equal(fs.pending.size, 0);
});
test('disconnect rejects outstanding reads', async () => {
  const fs = new JsFs();
  fs.attach(() => {});
  const wait = fs.wait('7', 0, 64);
  fs.receive(null);
  await assert.rejects(wait, /closed/);
  assert.equal(fs.pending.size, 0);
});
