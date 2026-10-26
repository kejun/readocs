const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
let clock = 0, requests = [], responses = [];
const elements = new Map();
function element() {
  return {value: '', textContent: '', disabled: false, hidden: false, children: [],
    replaceChildren() { this.children = []; }, append(child) { this.children.push(child); }, focus() {}};
}
const document = {
  querySelector(selector) { if (!elements.has(selector)) elements.set(selector, element()); return elements.get(selector); },
  querySelectorAll() { return [...elements.values()]; },
  createElement: element,
};
const context = vm.createContext({document, AbortController, confirm: () => true,
  Date: {now: () => clock},
  setTimeout(callback, delay) { if (delay === 1000) { clock += delay; queueMicrotask(callback); } return 1; },
  clearTimeout() {},
  async fetch(path, options) {
    requests.push({path, options});
    if (!responses.length) throw Error('disconnected');
    const response = responses.shift();
    if (response instanceof Error) throw response;
    return {ok: response.status < 400, status: response.status,
      headers: {get: () => response.type ?? 'application/json'},
      text: async () => typeof response.body === 'string' ? response.body : JSON.stringify(response.body)};
  },
});
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), context);
vm.runInContext("session='current';globalThis.api={call,poll,setBusy,load};", context);
const api = context.api;
const response = body => ({status: 200, body});
const state = (state, pending = false, operation = 1) => response({session: 'current', networks: [], connection: {state, pending, operation, message: state}});
(async () => {
  responses = [{status: 403, body: {message: '会话已过期'}}];
  await assert.rejects(api.call('/api/wifi', {}), /会话已过期/);
  responses = [{status: 503, type: 'text/plain', body: 'Network busy'}];
  await assert.rejects(api.call('/api/wifi', {}), /Network busy/);
  responses = [{status: 500, body: '{broken'}];
  await assert.rejects(api.call('/api/wifi', {}), /响应不完整/);
  responses = [state('connecting', true), state('failed')];
  await assert.rejects(api.poll(1, true), /failed/);
  requests = []; responses = [state('success'), response({accepted: true})];
  await api.poll(1, true);
  assert.deepEqual(requests.map(r => r.path), ['/api/status', '/api/complete']);
  assert.equal(requests[1].options.headers['X-Setup-Session'], 'current');
  assert.match(document.querySelector('#result').textContent, /已连接并保存/);
  requests = []; responses = []; clock = 0;
  await assert.rejects(api.poll(1, true), /尚未取得明确/);
  assert.equal(requests.length, 75); // Disconnection never counts as success or polls forever.
  responses = [response({session: 'new', connection: {operation: 1}})];
  await assert.rejects(api.poll(1, true), /会话已更换/);
  responses = [state('idle')];
  await api.poll(1, false); // Forgetting a network does not wait for Wi-Fi success.
  vm.runInContext("profiles=['a','b','c','d','e'];showProfiles();", context);
  document.querySelector('#ssid').value = 'sixth';
  document.querySelector('#password').value = 'password';
  requests = [];
  await document.querySelector('#form').onsubmit({preventDefault() {}});
  assert.equal(requests.length, 0);
  assert.match(document.querySelector('#result').textContent, /要替换/);
  document.querySelector('#replacement').value = 'b';
  responses = [response({accepted: true, operation: 1}), state('failed')];
  await document.querySelector('#form').onsubmit({preventDefault() {}});
  assert.equal(JSON.parse(requests[0].options.body).replace, 'b');
  assert.equal(document.querySelector('#password').disabled, false); // Failure restores editable controls.
  api.setBusy(true); requests = [];
  await document.querySelector('#form').onsubmit({preventDefault() {}});
  assert.equal(requests.length, 0);
})().catch(error => { console.error(error); process.exitCode = 1; });
