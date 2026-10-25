"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

// Run the production functions with controllable fetch completions and a minimal DOM.
const source = fs.readFileSync(
  path.join(__dirname, "../proxy/readocs/static/app.js"),
  "utf8",
);
const functions = source.slice(
  source.indexOf("function clearSource()"),
  source.indexOf("async function busy("),
);
assert(functions.includes("async function openSource("));
assert(functions.includes("async function openDocument("));

const context = vm.createContext({ assert, URL });
vm.runInContext(
  `
  let selectedSource = "a", previewId = null;
  let sourceRequestId = 0, previewRequestId = 0, fontSize = 18;
  const state = {
    sources: [{id: "a"}, {id: "b"}], feishu: {user_authorized: true}, jobs: []
  };
  const messages = {}, requests = [], notices = [], nodes = new Map(), elements = [];
  class Element {
    constructor(tag = "div", cls = "", text = "") {
      this.tagName = tag;
      this.className = cls;
      this.textContent = text;
      this.children = [];
      this.hidden = true;
      this.style = {};
      this.attributes = new Map();
      this.classList = { toggle() {} };
      elements.push(this);
    }
    replaceChildren(...children) {
      this.children.forEach(child => child.parent = null);
      this.children = [];
      this.append(...children);
    }
    append(...children) {
      children.forEach(child => { child.parent = this; this.children.push(child); });
    }
    setAttribute(name, value) { this.attributes.set(name, value); }
    removeAttribute(name) { this.attributes.delete(name); }
    remove() {
      if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this);
      this.parent = null;
    }
  }
  const $ = selector => {
    if (!nodes.has(selector)) nodes.set(selector, new Element());
    return nodes.get(selector);
  };
  const el = (tag, cls, text) => new Element(tag, cls, text);
  const document = {
    querySelectorAll: () => elements.filter(element => element.className === "article-row")
  };
  const api = url => new Promise((resolve, reject) => requests.push({url, resolve, reject}));
  const notify = text => notices.push(text);
  const busy = (button, fn) => fn();
  ${functions}
  `,
  context,
);

vm.runInContext(
  `(async () => {
    const a = {id: "a", title: "Source A", kind: "rss"};
    const b = {id: "b", title: "Source B", kind: "rss"};
    const empty = {documents: [], next_cursor: null};

    const olderSource = openSource(a), newerSource = openSource(b);
    requests[1].resolve(empty);
    await newerSource;
    requests[0].resolve(empty);
    await olderSource;
    assert.equal(selectedSource, "b");
    assert.equal($("#article-source").textContent, "Source B");

    const olderPreview = openDocument({snapshot_id: "old"}, null);
    const newerPreview = openDocument({snapshot_id: "new"}, null);
    requests[3].resolve({title: "New choice", blocks: []});
    await newerPreview;
    requests[2].resolve({title: "Old choice", blocks: []});
    await olderPreview;
    assert.equal(previewId, "new");
    assert.equal($("#reader").children[0].textContent, "New choice");

    const clearedPreview = openDocument({snapshot_id: "cleared"}, null);
    clearPreview();
    requests[4].resolve({title: "Must stay hidden", blocks: []});
    await clearedPreview;
    assert.equal(previewId, null);
    assert.equal($("#reader").hidden, true);

    const switchedPreview = openDocument({snapshot_id: "previous-source"}, null);
    const switchedSource = openSource(a);
    requests[6].resolve(empty);
    await switchedSource;
    requests[5].resolve({title: "Wrong source", blocks: []});
    await switchedPreview;
    assert.equal(previewId, null);
    assert.equal($("#reader").hidden, true);

    const removedSource = openSource(a);
    clearSource();
    requests[7].resolve({documents: [{title: "Removed source document", snapshot_id: "removed"}]});
    await removedSource;
    assert.equal(selectedSource, null);
    assert.equal($("#article-panel").hidden, true);
    assert.equal($("#article-list").children.length, 0);
    assert.equal(requests.length, 8, "Removed source must not start a preview");

    const firstSameSource = openSource(a), secondSameSource = openSource(a);
    requests[9].resolve({documents: [{title: "Current list", snapshot_id: null}], next_cursor: null});
    await secondSameSource;
    requests[8].resolve({documents: [{title: "Stale list", snapshot_id: null}], next_cursor: null});
    await firstSameSource;
    assert.equal($("#article-list").children[0].textContent, "Current list");

    appendDocuments({documents: [], next_cursor: 50}, $("#article-list"), "a");
    const more = $("#article-list").children.at(-1);
    const oldPagination = more.onclick();
    const reopened = openSource(a);
    requests[11].resolve({documents: [{title: "Reopened list", snapshot_id: null}], next_cursor: null});
    await reopened;
    requests[10].resolve({documents: [{title: "Old next page", snapshot_id: null}], next_cursor: null});
    await oldPagination;
    assert.equal($("#article-list").children.length, 1);
    assert.equal($("#article-list").children[0].textContent, "Reopened list");

    renderReaderMeta({url: "https://example.com/article", truncated: true});
    const original = $("#reader-meta").children.at(-1);
    assert.equal(original.tagName, "a");
    assert.equal(original.href, "https://example.com/article");
    assert.equal(original.target, "_blank");
    assert.equal(original.rel, "noopener noreferrer");
    assert.equal($("#reader-meta").children[0].textContent, "内容已截断");
    renderReaderMeta({url: "javascript:alert(1)"});
    assert.equal($("#reader-meta").children.filter(node => node.tagName === "a").length, 0);
    await openDocument({snapshot_id: null, url: "https://example.com/expired"}, null);
    assert.equal($("#reader-meta").children.at(-1).href, "https://example.com/expired");
    assert.equal($("#reader").hidden, true);
    clearPreview();
    assert.equal($("#reader-meta").children.length, 0);
  })()`,
  context,
).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
