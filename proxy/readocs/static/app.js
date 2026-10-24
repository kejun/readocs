"use strict";
const $ = (s) => document.querySelector(s);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};
let state = null,
  selectedSource = null,
  previewId = null,
  fontSize = 18,
  polling = null,
  activeRequests = 0;
const messages = {
  SOURCE_PARTIAL: "部分目录或文档同步失败 · 查看详情",
  USER_AUTH_REQUIRED: "请重新授权飞书账号",
  SOURCE_ACCESS_DENIED: "文档无访问权限 · 在飞书中授权后重试",
  APP_CREDENTIALS_INVALID: "应用凭据无效 · 请检查飞书配置",
  UPSTREAM_AUTH_REQUIRED: "需要修复飞书授权",
  UPSTREAM_TIMEOUT: "暂时无法连接 · 旧内容已保留",
  PARSE_ERROR: "源格式无法解析 · 请检查订阅地址",
  SOURCE_GONE: "源不可用 · 旧内容已保留",
  CONTENT_TOO_LARGE: "内容超过首版处理限制",
  SOURCE_UNSUPPORTED: "暂不支持此内容格式",
  RATE_LIMITED: "上游暂时限流 · 稍后重试",
  REVISION_CHANGED: "文档正在变化 · 稍后重试",
  URL_BLOCKED: "源地址不支持 · 请使用公开网络地址",
  INTERNAL_ERROR: "同步未完成 · 请重试",
};
async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      "X-Readocs-Request": "1",
      ...(options.headers || {}),
    },
    credentials: "same-origin",
  });
  let data = {};
  try {
    data = await response.json();
  } catch {}
  if (!response.ok) {
    if (response.status === 401 && path != "/api/login") showLogin();
    throw new Error(data.message || "操作未完成，请重试");
  }
  return data;
}
function notify(text) {
  $("#notice").textContent = text;
  $("#notice").hidden = false;
  clearTimeout(notify.timer);
  notify.timer = setTimeout(() => ($("#notice").hidden = true), 3500);
}
function showLogin() {
  $("#shell").hidden = true;
  $("#login").hidden = false;
  if (polling) {
    clearInterval(polling);
    polling = null;
  }
}
function showShell() {
  $("#login").hidden = true;
  $("#shell").hidden = false;
  route();
  if (!polling)
    polling = setInterval(() => {
      if (document.visibilityState === "visible" && !activeRequests)
        loadState().catch(() => {});
    }, 6000);
}
function route() {
  const page = ["library", "feishu", "device"].includes(location.hash.slice(1))
    ? location.hash.slice(1)
    : "library";
  for (const p of ["library", "feishu", "device"])
    $("#" + p + "-page").hidden = p !== page;
  document.querySelectorAll("[data-page]").forEach((a) => {
    if (a.dataset.page === page) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });
}
function relative(t) {
  if (!t) return "等待首次同步";
  let seconds = Math.max(0, Date.now() / 1000 - t);
  return seconds < 60
    ? "刚刚同步"
    : seconds < 3600
      ? `${Math.floor(seconds / 60)} 分钟前同步`
      : `${Math.floor(seconds / 3600)} 小时前同步`;
}
async function loadState(initial = false) {
  const data = await api("/api/state");
  state = data;
  renderSources();
  renderDevices();
  if (initial) {
    $("#app-id").value = data.feishu.app_id;
  }
  $("#feishu-callback").textContent = location.origin + "/api/feishu/callback";
  $("#authorize-feishu").hidden = !data.feishu.secret_set;
  $("#authorize-feishu").textContent = data.feishu.user_authorized
    ? "重新授权飞书账号"
    : "授权飞书账号";
  $("#secret-state").textContent = data.feishu.secret_set ? "已设置" : "";
  $("#app-secret").required = !data.feishu.secret_set;
  $("#feishu-details").hidden = !data.feishu.secret_set;
  const dl = $("#feishu-status");
  dl.replaceChildren();
  if (data.feishu.secret_set) {
    for (const [k, v] of [
      ["应用", data.feishu.app_id],
      [
        "账号",
        data.feishu.user_authorized
          ? "已授权"
          : "未授权，当前仅同步应用可访问目录",
      ],
      [
        "令牌有效至",
        data.feishu.expires
          ? new Date(data.feishu.expires * 1000).toLocaleString()
          : "等待下一次请求",
      ],
    ])
      dl.append(el("dt", "", k), el("dd", "", v));
  }
  return data;
}
function renderSources() {
  const list = $("#source-list");
  const opened = new Set(
    [...list.querySelectorAll("details[open]")].map((d) => d.dataset.sourceId),
  );
  list.replaceChildren();
  $(".reader-column").hidden = state.sources.length === 0;
  $("#library-empty").hidden = state.sources.length > 0;
  $("#add-feed").disabled =
    state.sources.filter((s) => s.kind === "rss").length >= 2;
  for (const s of state.sources) {
    const row = el("div", "source-row");
    row.append(el("span", "source-icon", s.kind === "feishu" ? "▤" : "◔"));
    const open = el("button", "source-body");
    if (selectedSource === s.id) open.setAttribute("aria-current", "true");
    open.append(el("span", "source-title", s.title));
    let status =
      s.kind === "feishu" && !state.feishu.user_authorized
        ? "请授权飞书账号"
        : s.error
          ? messages[s.error] || "同步失败 · 请重试"
          : state.jobs.some((j) => j.source_id === s.id)
            ? "正在同步…"
            : relative(s.fetched);
    open.append(
      el("span", "source-status" + (s.error ? " error" : ""), status),
    );
    open.onclick = () => openSource(s);
    row.append(open);
    const actions = el("div", "row-actions");
    const refresh = el("button", "quiet", "↻");
    refresh.setAttribute("aria-label", "检查 " + s.title + " 更新");
    refresh.onclick = async () => {
      refresh.disabled = true;
      try {
        await api("/api/sources/" + s.id + "/refresh", { method: "POST" });
        await loadState();
      } catch (e) {
        notify(e.message);
      } finally {
        refresh.disabled = false;
      }
    };
    actions.append(refresh);
    row.append(actions);
    const details = el("details", "source-details");
    details.dataset.sourceId = s.id;
    details.open = opened.has(s.id);
    details.append(el("summary", "", "详情"));
    const inner = el("div");
    if (s.url) inner.append(el("p", "", s.url));
    inner.append(
      el("p", "", `${s.count} 篇内容 · 每 ${s.interval / 60} 分钟检查`),
    );
    if (s.progress) {
      const p = s.progress;
      inner.append(
        el(
          "p",
          "",
          `发现 ${p.discovered} · 已同步 ${p.synced} · 失败 ${p.failed}${p.scanning ? " · 正在发现文档" : ""}`,
        ),
      );
    }
    if (s.kind === "rss") {
      const remove = el("button", "quiet danger", "移除订阅");
      remove.onclick = async () => {
        if (!confirm("移除此订阅？设备上已经下载的内容仍可离线阅读。")) return;
        try {
          await api("/api/sources/" + s.id, { method: "DELETE" });
          await loadState();
          if (selectedSource === s.id) {
            $("#article-panel").hidden = true;
            clearPreview();
          }
        } catch (e) {
          notify(e.message);
        }
      };
      inner.append(remove);
    }
    details.append(inner);
    row.append(details);
    list.append(row);
  }
}
function renderDevices() {
  const list = $("#device-list");
  list.replaceChildren();
  const devices = state.devices.filter((d) => !d.revoked);
  $("#device-empty").hidden = devices.length > 0;
  for (const d of devices) {
    const row = el("div", "device-row"),
      info = el("div");
    info.append(
      el("h2", "", d.name),
      el(
        "p",
        "",
        d.last_seen
          ? "最近连接 " + new Date(d.last_seen * 1000).toLocaleString()
          : "等待设备首次同步",
      ),
    );
    const b = el("button", "quiet danger", "解除绑定");
    b.onclick = async () => {
      if (!confirm("解除此设备的同步权限？")) return;
      try {
        await api("/api/devices/" + d.id, { method: "DELETE" });
        await loadState();
      } catch (e) {
        notify(e.message);
      }
    };
    row.append(info, b);
    list.append(row);
  }
}
function clearPreview() {
  previewId = null;
  $("#reader").hidden = true;
  $("#reader-placeholder").hidden = false;
  $("#reader-meta").textContent = "";
}
async function openSource(source) {
  selectedSource = source.id;
  try {
    const result = await api("/api/sources/" + source.id + "/documents");
    $("#article-source").textContent = source.title;
    $("#article-source").hidden = state.sources.length === 1;
    $("#article-panel .list-heading").classList.toggle(
      "single-source",
      state.sources.length === 1,
    );
    const list = $("#article-list");
    list.replaceChildren();
    $("#article-panel").hidden = false;
    if (!result.documents.length) {
      list.append(
        el(
          "p",
          "source-status",
          source.error
            ? messages[source.error] || "同步未完成，请重试"
            : source.kind === "feishu" && !state.feishu.user_authorized
              ? "在飞书应用中授权账号后同步个人文档"
              : state.jobs.some((j) => j.source_id === source.id)
                ? "内容正在同步，请稍候。"
                : "暂无可同步的内容",
        ),
      );
      clearPreview();
      return;
    }
    appendDocuments(result, list, source.id);
    const first = result.documents.find((d) => d.snapshot_id);
    if (first)
      await openDocument(first, list.children[result.documents.indexOf(first)]);
  } catch (e) {
    notify(e.message);
  }
}
function appendDocuments(result, list, sourceId) {
  for (const d of result.documents) {
    const b = el("button", "article-row", d.title);
    if (d.sync_error)
      b.append(el("span", "error", messages[d.sync_error] || "同步失败"));
    if (d.summary) b.append(el("span", "", "摘要"));
    if (!d.snapshot_id && !d.sync_error)
      b.append(el("span", "", sourceId === "feishu" ? "待同步" : "正文已过期"));
    b.onclick = () => openDocument(d, b);
    list.append(b);
  }
  if (result.next_cursor !== null && result.next_cursor !== undefined) {
    const more = el("button", "secondary", "加载更多");
    more.onclick = () =>
      busy(more, async () => {
        try {
          const next = await api(
            `/api/sources/${sourceId}/documents?cursor=${result.next_cursor}`,
          );
          if (selectedSource !== sourceId) return;
          more.remove();
          appendDocuments(next, list, sourceId);
        } catch (e) {
          notify(e.message);
        }
      });
    list.append(more);
  }
}
async function openDocument(doc, button) {
  if (!doc.snapshot_id) {
    notify(
      selectedSource === "feishu"
        ? "正文尚未同步，请稍后重试"
        : "正文已过期，请查看原文",
    );
    return;
  }
  try {
    const data = await api("/api/preview/" + doc.snapshot_id);
    previewId = doc.snapshot_id;
    const reader = $("#reader");
    reader.replaceChildren(el("h1", "", data.title));
    for (const b of data.blocks) {
      let t =
        b.type === "h1"
          ? "h2"
          : b.type === "h2" || b.type === "h3"
            ? "h3"
            : b.type === "code"
              ? "pre"
              : b.type === "quote"
                ? "blockquote"
                : b.type === "separator"
                  ? "hr"
                  : "p";
      const node = el(
        t,
        b.type === "placeholder" ? "placeholder" : "",
        t === "hr"
          ? undefined
          : (b.type === "bullet" ? "• " : b.type === "ordered" ? "1. " : "") +
              b.text,
      );
      if (b.depth) node.style.marginLeft = Math.min(b.depth, 4) * 12 + "px";
      reader.append(node);
    }
    reader.style.fontSize = fontSize + "px";
    reader.hidden = false;
    $("#reader-placeholder").hidden = true;
    $("#reader-meta").textContent = [
      data.summary ? "摘要" : "",
      data.truncated ? "内容已截断" : "",
    ]
      .filter(Boolean)
      .join(" · ");
    document
      .querySelectorAll(".article-row")
      .forEach((b) => b.removeAttribute("aria-current"));
    button?.setAttribute("aria-current", "true");
  } catch (e) {
    notify(e.message);
  }
}
async function busy(button, fn) {
  activeRequests++;
  button.disabled = true;
  try {
    await fn();
  } finally {
    button.disabled = false;
    activeRequests--;
  }
}
$("#login-form").onsubmit = (e) => {
  e.preventDefault();
  busy(e.submitter, async () => {
    try {
      await api("/api/login", {
        method: "POST",
        body: JSON.stringify({ key: $("#admin-key").value }),
      });
      $("#admin-key").value = "";
      $("#login-error").textContent = "";
      await loadState(true);
      showShell();
    } catch (err) {
      $("#login-error").textContent = err.message;
    }
  });
};
$("#logout").onclick = async () => {
  try {
    await api("/api/logout", { method: "POST" });
    showLogin();
  } catch (e) {
    notify(e.message);
  }
};
function feishuPayload() {
  const p = {
    app_id: $("#app-id").value.trim(),
  };
  if ($("#app-secret").value) p.secret = $("#app-secret").value;
  return p;
}
$("#authorize-feishu").onclick = (e) =>
  busy(e.currentTarget, async () => {
    try {
      const auth = await api("/api/feishu/authorize", { method: "POST" });
      location.assign(auth.url);
    } catch (err) {
      $("#feishu-result").classList.add("error");
      $("#feishu-result").textContent = err.message;
    }
  });
$("#test-feishu").onclick = (e) =>
  busy(e.currentTarget, async () => {
    const r = $("#feishu-result");
    r.classList.remove("error");
    r.textContent = "正在验证…";
    try {
      await api("/api/feishu/test", {
        method: "POST",
        body: JSON.stringify(feishuPayload()),
      });
      r.textContent = "应用凭据通过";
    } catch (err) {
      r.classList.add("error");
      r.textContent = err.message;
    }
  });
$("#feishu-form").onsubmit = (e) => {
  e.preventDefault();
  busy(e.submitter, async () => {
    const r = $("#feishu-result");
    r.classList.remove("error");
    r.textContent = "正在验证…";
    try {
      await api("/api/feishu", {
        method: "PUT",
        body: JSON.stringify(feishuPayload()),
      });
      $("#app-secret").value = "";
      r.textContent = "已保存，正在发现全部可访问文档";
      await loadState();
    } catch (err) {
      r.classList.add("error");
      r.textContent = err.message;
    }
  });
};
$("#remove-feishu").onclick = async () => {
  if (!confirm("移除飞书应用和文档配置？RSS 和设备绑定会保留。")) return;
  try {
    await api("/api/feishu", { method: "DELETE" });
    $("#feishu-form").reset();
    $("#feishu-result").textContent = "已移除";
    await loadState();
  } catch (e) {
    notify(e.message);
  }
};
$("#add-feed").onclick = () => {
  $("#feed-error").textContent = "";
  $("#feed-form").reset();
  $("#feed-dialog").showModal();
};
$("#cancel-feed").onclick = () => $("#feed-dialog").close();
$("#feed-form").onsubmit = (e) => {
  e.preventDefault();
  busy(e.submitter, async () => {
    try {
      await api("/api/feeds", {
        method: "POST",
        body: JSON.stringify({
          url: $("#feed-url").value.trim(),
          interval: Number($("#feed-interval").value),
        }),
      });
      $("#feed-dialog").close();
      await loadState();
    } catch (err) {
      $("#feed-error").textContent = err.message;
    }
  });
};
$("#pair-device").onclick = (e) =>
  busy(e.currentTarget, async () => {
    try {
      const p = await api("/api/pairings", { method: "POST" });
      $("#pair-panel").hidden = false;
      $("#pair-code").textContent = p.code;
      $("#pair-expiry").textContent = "5 分钟内有效，仅可使用一次";
    } catch (err) {
      notify(err.message);
    }
  });
$("#close-articles").onclick = () => ($("#article-panel").hidden = true);
$("#font-smaller").onclick = () => {
  fontSize = Math.max(14, fontSize - 2);
  $("#reader").style.fontSize = fontSize + "px";
};
$("#font-larger").onclick = () => {
  fontSize = Math.min(30, fontSize + 2);
  $("#reader").style.fontSize = fontSize + "px";
};
window.addEventListener("hashchange", route);
loadState(true).then(showShell).catch(showLogin);

const authResult = new URLSearchParams(location.search).get("feishu_auth");
if (authResult) {
  $("#feishu-result").textContent =
    authResult === "ok"
      ? "账号已授权，正在同步全部文档"
      : "授权未完成，请检查应用权限和回调地址后重试";
  $("#feishu-result").classList.toggle("error", authResult !== "ok");
  history.replaceState(null, "", "/#feishu");
}
