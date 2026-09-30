/* m5-uarch results viewer. No dependencies; data comes from data.js. */
(function () {
  "use strict";
  var DATA = window.UARCH_DATA || { chips: [], references: [] };
  var $ = function (id) { return document.getElementById(id); };
  var chip = null;

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    for (var k in attrs || {}) {
      if (k === "text") e.textContent = attrs[k];
      else if (k === "cls") e.className = attrs[k];
      else e.setAttribute(k, attrs[k]);
    }
    (children || []).forEach(function (c) { if (c) e.appendChild(c); });
    return e;
  }

  function fmt(v, digits) {
    if (v === null || v === undefined) return "";
    var d = digits === undefined ? 2 : digits;
    var s = Number(v).toFixed(d);
    if (s.indexOf(".") >= 0) s = s.replace(/0+$/, "").replace(/\.$/, "");
    return s === "-0" ? "0" : s;
  }

  /* Latency summary for one core: a single value, or a range over the paths. */
  function latSummary(core) {
    if (!core) return { text: "", flag: "" };
    if (core.unsupported) return { text: "n/a", flag: "" };
    var vals = (core.lat || []).filter(function (c) { return c; });
    if (!vals.length) return { text: "", flag: "" };
    var lo = Infinity, hi = -Infinity, rt = false;
    vals.forEach(function (c) {
      lo = Math.min(lo, c[0]); hi = Math.max(hi, c[0]);
      if (c.length > 3) rt = true;
    });
    var text = Math.abs(hi - lo) < 0.05 ? fmt(hi) : fmt(lo) + "–" + fmt(hi);
    return { text: text, flag: rt ? " rt" : "" };
  }

  function tpSummary(core) {
    if (!core) return { text: "", flag: "" };
    if (core.unsupported) return { text: "n/a", flag: "" };
    if (!core.tp) return { text: core.tp_status ? "(" + core.tp_status + ")" : "", flag: "" };
    return { text: fmt(core.tp[0]), flag: core.tp_chain_bound ? " *" : "" };
  }

  function numCell(s) {
    var td = el("td", { cls: "num", text: s.text });
    if (s.flag) td.appendChild(el("span", { cls: "flag", text: s.flag }));
    return td;
  }

  function rangeText(t) {
    if (!t) return "";
    if (Math.abs(t[2] - t[1]) < 0.0005) return "same in every run";
    return "runs: " + fmt(t[1], 3) + " to " + fmt(t[2], 3);
  }

  function insnDetail(ins) {
    var dl = el("dl");
    function add(k, v) { dl.appendChild(el("dt", { text: k })); dl.appendChild(v); }
    add("name", el("dd", {}, [el("code", { text: ins.name })]));
    add("group", el("dd", { text: ins.group + (ins.ext ? " (" + ins.ext + ")" : "") }));
    if (ins.note) add("note", el("dd", { text: ins.note }));
    chip.cores.forEach(function (c) {
      var core = ins[c];
      if (!core) return;
      if (core.unsupported) {
        add(c + "-core", el("dd", { text: "could not be executed (signal " + core.signal + ")" }));
        return;
      }
      if (core.tp) {
        add(c + " throughput", el("dd", {
          text: fmt(core.tp[0], 3) + " per cycle (" + rangeText(core.tp) + ")" +
            (core.tp_chain_bound ? "; limited by the block's own dependency chain, so this is a latency, not a port count" : "")
        }));
      }
      (ins.paths || []).forEach(function (p, i) {
        var cell = (core.lat || [])[i];
        if (!cell) return;
        var dd = el("dd");
        dd.appendChild(document.createTextNode(fmt(cell[0], 3) + " cycles (" + rangeText(cell) + ")"));
        if (cell.length > 3) dd.appendChild(el("span", { cls: "flag", text: " round trip through " + (p.via || "a helper") }));
        if (p.tied) dd.appendChild(el("span", { cls: "dim", text: " input tied to the read-write operand" }));
        dd.appendChild(el("br"));
        dd.appendChild(el("code", { cls: "dim", text: p.chain }));
        add(c + " latency " + p.from + " → " + p.to, dd);
      });
    });
    return dl;
  }

  function curveSvg(e) {
    var pts = e.curve;
    if (!pts || pts.length < 2) return null;
    var W = 620, H = 180, L = 46, R = 10, T = 8, B = 40;
    var xs = pts.map(function (p) { return p[0]; }), ys = pts.map(function (p) { return p[1]; });
    var logx = Math.min.apply(null, xs) > 0 && Math.max.apply(null, xs) / Math.min.apply(null, xs) > 30;
    var fx = function (x) { return logx ? Math.log(x) : x; };
    var x0 = fx(Math.min.apply(null, xs)), x1 = fx(Math.max.apply(null, xs));
    var y0 = Math.min(0, Math.min.apply(null, ys)), y1 = Math.max.apply(null, ys) * 1.05 || 1;
    var px = function (x) { return L + (fx(x) - x0) / ((x1 - x0) || 1) * (W - L - R); };
    var py = function (y) { return H - B - (y - y0) / ((y1 - y0) || 1) * (H - T - B); };
    var ns = "http://www.w3.org/2000/svg";
    var svg = document.createElementNS(ns, "svg");
    svg.setAttribute("class", "curve"); svg.setAttribute("viewBox", "0 0 " + W + " " + H);
    svg.setAttribute("width", W); svg.setAttribute("height", H);
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label", e.ylabel + " against " + e.xlabel);
    function node(name, attrs, text) {
      var n = document.createElementNS(ns, name);
      for (var k in attrs) n.setAttribute(k, attrs[k]);
      if (text !== undefined) n.textContent = text;
      svg.appendChild(n);
    }
    node("line", { x1: L, y1: H - B, x2: W - R, y2: H - B });
    node("line", { x1: L, y1: T, x2: L, y2: H - B });
    node("path", { d: pts.map(function (p, i) { return (i ? "L" : "M") + px(p[0]).toFixed(1) + " " + py(p[1]).toFixed(1); }).join(" ") });
    pts.forEach(function (p) { node("circle", { cx: px(p[0]).toFixed(1), cy: py(p[1]).toFixed(1), r: 1.8 }); });
    node("text", { x: L, y: H - B + 14 }, fmt(Math.min.apply(null, xs)));
    node("text", { x: W - R, y: H - B + 14, "text-anchor": "end" }, fmt(Math.max.apply(null, xs)));
    node("text", { x: (L + W - R) / 2, y: H - 8, "text-anchor": "middle" }, e.xlabel + (logx ? " (log scale)" : ""));
    node("text", { x: L - 4, y: py(y1 / 1.05) + 4, "text-anchor": "end" }, fmt(y1 / 1.05));
    node("text", { x: L - 4, y: py(y0) + 4, "text-anchor": "end" }, fmt(y0));
    node("text", { x: L + 4, y: T + 8 }, e.ylabel);
    return svg;
  }

  function toggleDetail(tr, build, cols) {
    var next = tr.nextSibling;
    if (next && next.className === "detail") { next.remove(); return; }
    var td = el("td", { colspan: cols });
    build(td);
    tr.parentNode.insertBefore(el("tr", { cls: "detail" }, [td]), tr.nextSibling);
  }

  function structValue(e) {
    if (!e) return { text: "", flag: "" };
    if (e.status !== "ok") return { text: "—", flag: " ?" };
    var digits = e.unit === "entries" || e.unit === "bytes" ? 0 : 2;
    return { text: fmt(e.value, digits), flag: e.confidence === "high" ? "" : " (" + e.confidence + ")" };
  }

  function refText(id) {
    var parts = [];
    (DATA.references || []).forEach(function (r) {
      var v = (r.structure || {})[id];
      if (v) parts.push(r.chip + ": " + v.value + (v.note ? " (" + v.note + ")" : ""));
    });
    return parts.join("; ");
  }

  function renderStructure() {
    var body = $("structure").tBodies[0];
    body.textContent = "";
    var ids = [], byCore = {};
    chip.cores.forEach(function (c) {
      byCore[c] = {};
      (chip.structure[c] || []).forEach(function (e) {
        byCore[c][e.id] = e;
        if (ids.indexOf(e.id) < 0) ids.push(e.id);
      });
    });
    var anyRef = false;
    ids.forEach(function (id) {
      var p = byCore.P && byCore.P[id], e2 = byCore.E && byCore.E[id];
      var any = p || e2 || byCore[chip.cores[0]][id];
      var ref = refText(id);
      if (ref) anyRef = true;
      var tr = el("tr", { cls: "row" }, [
        el("td", { text: any.title }), numCell(structValue(p)), numCell(structValue(e2)),
        el("td", { cls: "dim", text: any.unit }), el("td", { cls: "dim", text: ref })
      ]);
      tr.addEventListener("click", function () {
        toggleDetail(tr, function (td) {
          chip.cores.forEach(function (c) {
            var e = byCore[c][id];
            if (!e) return;
            var head = c + "-core: " + (e.status === "ok"
              ? fmt(e.value, 3) + " " + e.unit + ", confidence " + e.confidence +
                (e.min !== undefined && e.min !== e.max ? ", runs " + fmt(e.min, 3) + " to " + fmt(e.max, 3) : "")
              : e.status);
            td.appendChild(el("p", {}, [el("b", { text: head })]));
            td.appendChild(el("p", { text: e.note || "" }));
            var svg = curveSvg(e);
            if (svg) td.appendChild(svg);
          });
        }, 5);
      });
      body.appendChild(tr);
    });
    $("refhead").textContent = anyRef ? "Published for other chips" : "";
    var src = $("refsrc");
    src.textContent = "";
    (DATA.references || []).forEach(function (r) {
      src.appendChild(el("li", {}, [document.createTextNode(r.chip + ": " + r.source + ", "), el("a", { href: r.url, text: r.url })]));
    });
  }

  var rows = [];
  function renderInsns() {
    var body = $("insns").tBodies[0];
    body.textContent = "";
    rows = [];
    var groups = [], sel = $("group");
    sel.length = 1;
    var last = null;
    chip.instructions.forEach(function (ins) {
      if (ins.group === "Chain helpers") return;
      if (ins.group !== last) {
        last = ins.group;
        groups.push(ins.group);
        var gh = el("tr", { cls: "grouphead" }, [el("td", { colspan: 6, text: ins.group })]);
        body.appendChild(gh);
        rows.push({ head: true, group: ins.group, tr: gh });
      }
      var tr = el("tr", { cls: "row" }, [
        el("td", {}, [el("span", { cls: "asm", text: ins.asm })]),
        el("td", { cls: "dim", text: ins.ext || "" }),
        numCell(latSummary(ins.P)), numCell(tpSummary(ins.P)),
        numCell(latSummary(ins.E)), numCell(tpSummary(ins.E))
      ]);
      tr.addEventListener("click", function () {
        toggleDetail(tr, function (td) { td.appendChild(insnDetail(ins)); }, 6);
      });
      body.appendChild(tr);
      rows.push({ tr: tr, group: ins.group,
        hay: (ins.name + " " + ins.asm + " " + ins.group + " " + (ins.ext || "") + " " + (ins.note || "")).toLowerCase() });
    });
    groups.forEach(function (g) { sel.appendChild(el("option", { value: g, text: g })); });
    filter();
  }

  function filter() {
    var words = $("q").value.toLowerCase().split(/\s+/).filter(Boolean), g = $("group").value;
    var shown = 0, groupShown = {};
    rows.forEach(function (r) {
      if (r.head) return;
      var ok = (!g || r.group === g) && words.every(function (w) { return r.hay.indexOf(w) >= 0; });
      r.tr.hidden = !ok;
      var d = r.tr.nextSibling;
      if (d && d.className === "detail") d.hidden = !ok;
      if (ok) { shown++; groupShown[r.group] = true; }
    });
    rows.forEach(function (r) { if (r.head) r.tr.hidden = !groupShown[r.group]; });
    $("count").textContent = shown + " of " + rows.filter(function (r) { return !r.head; }).length + " entries";
  }

  function slug(s) { return s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, ""); }

  function renderChip() {
    var m = chip.machine;
    var levels = m.levels.map(function (l) {
      return l.cores + " " + l.label + " (“" + l.name + "”" + (l.ghz_observed ? ", " + l.ghz_observed + " GHz while measuring" : "") + ")";
    }).join(" + ");
    var loads = chip.runs.map(function (r) { return r.load_average[0]; });
    $("machine").textContent = m.model + ", macOS " + m.os_version + ", " + levels + ". " +
      chip.runs.length + " runs merged; load average during them " + fmt(Math.min.apply(null, loads)) +
      " to " + fmt(Math.max.apply(null, loads)) + "." +
      (m.pauth_keys_active === false ? " Pointer-authentication keys were inactive in the measuring process, so pac*/aut* behaved as moves." : "");
    var s = slug(m.brand);
    $("json").href = "https://github.com/useless-husband/m5-uarch/blob/main/results/" + s + "/" + s + ".json";
    $("csv").href = "https://github.com/useless-husband/m5-uarch/tree/main/results/" + s;
    renderStructure();
    renderInsns();
  }

  function init() {
    var sel = $("chip");
    if (!DATA.chips.length) {
      $("machine").textContent = "No results found (data.js is missing or empty).";
      return;
    }
    DATA.chips.forEach(function (c, i) { sel.appendChild(el("option", { value: i, text: c.machine.brand })); });
    sel.addEventListener("change", function () { chip = DATA.chips[sel.value]; renderChip(); });
    $("q").addEventListener("input", filter);
    $("group").addEventListener("change", filter);
    chip = DATA.chips[0];
    renderChip();
  }
  init();
})();
