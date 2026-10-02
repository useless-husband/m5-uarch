/* m5-uarch results viewer. No dependencies.
 * data.js holds the chip list, datasets and structure figures; each chip's
 * instruction table is in data/<chip>.js and is loaded when the chip is shown.
 * A value cell is [value, lo, hi, n, flags]: n datasets agree on it; with one
 * dataset lo..hi is its run-to-run range, with several the range of the
 * datasets' medians. Flags: 1 round trip, 2 marked, 4 limited by own chain. */
(function () {
  "use strict";
  var INDEX = window.UARCH_INDEX || { chips: [], references: [] };
  var REPO = "https://github.com/" + (INDEX.repo || "useless-husband/m5-uarch");
  var F_RT = 1, F_MARK = 2, F_CHAIN = 4;
  var $ = function (id) { return document.getElementById(id); };
  var bySlug = {}, loaded = {}, pending = {};
  var selected = [];

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    for (var k in attrs || {}) {
      if (k === "text") e.textContent = attrs[k];
      else if (k === "cls") e.className = attrs[k];
      else e.setAttribute(k, attrs[k]);
    }
    (children || []).forEach(function (c) {
      if (c) e.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
    });
    return e;
  }

  function fmt(v, digits) {
    if (v === null || v === undefined) return "";
    var d = digits === undefined ? 2 : digits;
    var s = Number(v).toFixed(d);
    if (s.indexOf(".") >= 0) s = s.replace(/0+$/, "").replace(/\.$/, "");
    return s === "-0" ? "0" : s;
  }

  function short(chip) { return chip.brand.replace(/^Apple /, ""); }

  /* ---- loading chip tables ------------------------------------------- */
  window.UARCH_CHIP = function (t) {
    loaded[t.slug] = t;
    (pending[t.slug] || []).forEach(function (f) { f(); });
    delete pending[t.slug];
  };

  function loadChip(chip, done) {
    if (loaded[chip.slug]) { done(); return; }
    if (pending[chip.slug]) { pending[chip.slug].push(done); return; }
    pending[chip.slug] = [done];
    var s = document.createElement("script");
    s.src = chip.insns_file;
    s.onerror = function () { $("count").textContent = "Could not load " + chip.insns_file + "."; };
    document.head.appendChild(s);
  }

  /* ---- cells ---------------------------------------------------------- */
  function spreadText(c) {
    if (!c) return "";
    if (c[3] > 1) return c[3] + " datasets: " + fmt(c[1], 3) + " to " + fmt(c[2], 3);
    if (Math.abs(c[2] - c[1]) < 0.0005) return "same in every run";
    return "runs: " + fmt(c[1], 3) + " to " + fmt(c[2], 3);
  }

  function latSummary(core) {
    if (!core) return { text: "", flag: "" };
    if (core.unsupported) return { text: "n/a", flag: "" };
    var vals = (core.lat || []).filter(function (c) { return c; });
    if (!vals.length) return { text: "", flag: "" };
    var lo = Infinity, hi = -Infinity, rt = false, mark = false;
    vals.forEach(function (c) {
      lo = Math.min(lo, c[0]); hi = Math.max(hi, c[0]);
      if (c[4] & F_RT) rt = true;
      if (c[4] & F_MARK) mark = true;
    });
    var text = Math.abs(hi - lo) < 0.05 ? fmt(hi) : fmt(lo) + "–" + fmt(hi);
    return { text: text, flag: (rt ? " rt" : "") + (mark ? " †" : "") };
  }

  function tpSummary(core) {
    if (!core) return { text: "", flag: "" };
    if (core.unsupported) return { text: "n/a", flag: "" };
    if (!core.tp) return { text: core.tp_status ? "(" + core.tp_status + ")" : "", flag: "" };
    var f = core.tp[4];
    return { text: fmt(core.tp[0]), flag: (f & F_CHAIN ? " *" : "") + (f & F_MARK ? " †" : "") };
  }

  function numCell(s) {
    var td = el("td", { cls: "num", text: s.text });
    if (s.flag) td.appendChild(el("span", { cls: "flag", text: s.flag }));
    return td;
  }

  function structValue(row) {
    if (!row) return { text: "", flag: "" };
    if (!row.cell) return { text: "—", flag: " ?" };
    var digits = row.unit === "entries" || row.unit === "bytes" ? 0 : 2;
    var flag = (row.confidence === "high" ? "" : " (" + row.confidence + ")") +
      (row.cell[4] & F_MARK ? " †" : "");
    return { text: fmt(row.cell[0], digits), flag: flag };
  }

  function toggleDetail(tr, build, cols) {
    var next = tr.nextSibling;
    if (next && next.className === "detail") { next.remove(); return; }
    var td = el("td", { colspan: cols });
    build(td);
    tr.parentNode.insertBefore(el("tr", { cls: "detail" }, [td]), tr.nextSibling);
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

  /* ---- chip list ------------------------------------------------------ */
  var STATUS = {
    verified: "verified: independent submissions agree",
    single: "single submission",
    flagged: "flagged: datasets disagree",
    local: "local only, not submitted"
  };

  function renderChips() {
    var body = $("chips").tBodies[0];
    body.textContent = "";
    INDEX.chips.forEach(function (c) {
      var box = el("input", { type: "checkbox", "aria-label": "Show " + c.brand });
      box.checked = selected.indexOf(c.slug) >= 0;
      box.addEventListener("change", function () {
        if (box.checked) { if (selected.indexOf(c.slug) < 0) selected.push(c.slug); }
        else selected = selected.filter(function (s) { return s !== c.slug; });
        if (!selected.length) { selected = [c.slug]; box.checked = true; }
        selectionChanged();
      });
      var levels = c.levels.map(function (l) { return l.cores + " " + l.label + " (" + l.name + ")"; }).join(" + ");
      body.appendChild(el("tr", {}, [
        el("td", { cls: "pick" }, [box]),
        el("td", {}, [el("b", { text: c.brand })]),
        el("td", { cls: "dim", text: c.models.join(", ") }),
        el("td", { cls: "num", text: String(c.published || c.datasets.length) }),
        el("td", { cls: "status-" + c.status, text: STATUS[c.status] || c.status }),
        el("td", { cls: "dim", text: levels })
      ]));
    });
  }

  function chosen() { return selected.map(function (s) { return bySlug[s]; }).filter(Boolean); }

  function coreColumns(chips, only) {
    var cols = [];
    chips.forEach(function (chip) {
      chip.cores.forEach(function (core) {
        if (!only || only === core) cols.push({ chip: chip, core: core });
      });
    });
    return cols;
  }

  function colName(col, many) { return (many ? short(col.chip) + " " : "") + col.core; }

  /* ---- structure ------------------------------------------------------ */
  function refText(id) {
    var parts = [];
    (INDEX.references || []).forEach(function (r) {
      var v = (r.structure || {})[id];
      if (v) parts.push(r.chip + ": " + v.value + (v.note ? " (" + v.note + ")" : ""));
    });
    return parts.join("; ");
  }

  function datasetName(chip, i) {
    var d = chip.datasets[i];
    return d.id + " (" + d.model + (d.local ? ", local" : "") + ")";
  }

  function renderStructure() {
    var chips = chosen(), many = chips.length > 1;
    var cols = coreColumns(chips, "");
    var head = $("structure").tHead.rows[0];
    head.textContent = "";
    head.appendChild(el("th", { text: "What" }));
    cols.forEach(function (c) { head.appendChild(el("th", { cls: "num", text: colName(c, many) + (c.core.length === 1 ? "-core" : "") })); });
    head.appendChild(el("th", { text: "Unit" }));
    head.appendChild(el("th", { text: "Published for other chips" }));
    var body = $("structure").tBodies[0];
    body.textContent = "";
    var ids = [], titles = {}, units = {}, rows = {};
    cols.forEach(function (col) {
      (col.chip.structure[col.core] || []).forEach(function (r) {
        if (ids.indexOf(r.id) < 0) { ids.push(r.id); titles[r.id] = r.title; units[r.id] = r.unit; }
        rows[col.chip.slug + "|" + col.core + "|" + r.id] = r;
      });
    });
    var ncols = cols.length + 3;
    ids.forEach(function (id) {
      var cells = cols.map(function (col) { return numCell(structValue(rows[col.chip.slug + "|" + col.core + "|" + id])); });
      var tr = el("tr", { cls: "row" }, [el("td", { text: titles[id] })].concat(cells).concat([
        el("td", { cls: "dim nowrap", text: units[id] }), el("td", { cls: "dim", text: refText(id) })
      ]));
      tr.addEventListener("click", function () {
        toggleDetail(tr, function (td) {
          cols.forEach(function (col) {
            var r = rows[col.chip.slug + "|" + col.core + "|" + id];
            if (!r) return;
            var head = col.chip.brand + " " + col.core + "-core: " + (r.cell
              ? fmt(r.cell[0], 3) + " " + r.unit + ", confidence " + r.confidence + " (" + spreadText(r.cell) + ")"
              : r.status);
            td.appendChild(el("p", {}, [el("b", { text: head })]));
            if (r.per.length > 1) {
              var ul = el("ul", { cls: "per" });
              r.per.forEach(function (p) {
                ul.appendChild(el("li", {}, [datasetName(col.chip, p[0]) + ": " + (p[1] === null ? "inconclusive" : fmt(p[1], 3)) +
                  (p[3] ? ", " + p[3] : ""), p[2] ? el("span", { cls: "flag", text: " † differs from the others" }) : null]));
              });
              td.appendChild(ul);
            }
            td.appendChild(el("p", { text: r.note || "" }));
            var svg = curveSvg(r);
            if (svg) td.appendChild(svg);
          });
        }, ncols);
      });
      body.appendChild(tr);
    });
    var src = $("refsrc");
    src.textContent = "";
    (INDEX.references || []).forEach(function (r) {
      src.appendChild(el("li", {}, [r.chip + ": " + r.source + ", ", el("a", { href: r.url, text: r.url })]));
    });
  }

  /* ---- instructions --------------------------------------------------- */
  var rows = [];

  function insnDetail(ins, entries, cols) {
    var dl = el("dl");
    function add(k, v) { dl.appendChild(el("dt", { text: k })); dl.appendChild(v); }
    add("name", el("dd", {}, [el("code", { text: ins.name })]));
    add("group", el("dd", { text: ins.group + (ins.ext ? " (" + ins.ext + ")" : "") }));
    if (ins.note) add("note", el("dd", { text: ins.note }));
    var many = chosen().length > 1;
    cols.forEach(function (col) {
      var e = entries[col.chip.slug], core = e && e[col.core];
      var label = (many ? short(col.chip) + " " : "") + col.core;
      if (!core) return;
      if (core.unsupported) {
        add(label + "-core", el("dd", { text: "could not be executed (signal " + core.signal + ")" }));
        return;
      }
      if (core.tp) {
        add(label + " throughput", el("dd", {
          text: fmt(core.tp[0], 3) + " per cycle (" + spreadText(core.tp) + ")" +
            (core.tp[4] & F_CHAIN ? "; limited by the block's own dependency chain, so this is a latency, not a unit count" : "") +
            (core.tp[4] & F_MARK ? "; a dataset differs here (†)" : "")
        }));
      }
      (e.paths || []).forEach(function (p, i) {
        var cell = (core.lat || [])[i];
        if (!cell) return;
        var dd = el("dd");
        dd.appendChild(document.createTextNode(fmt(cell[0], 3) + " cycles (" + spreadText(cell) + ")"));
        if (cell[4] & F_RT) dd.appendChild(el("span", { cls: "flag", text: " round trip through " + (p.via || "a helper") }));
        if (cell[4] & F_MARK) dd.appendChild(el("span", { cls: "flag", text: " † a dataset differs here" }));
        if (p.tied) dd.appendChild(el("span", { cls: "dim", text: " input tied to the read-write operand" }));
        dd.appendChild(el("br"));
        dd.appendChild(el("code", { cls: "dim", text: p.chain }));
        add(label + " latency " + p.from + " → " + p.to, dd);
      });
    });
    return dl;
  }

  function renderInsns() {
    var chips = chosen(), many = chips.length > 1;
    var cols = coreColumns(chips, $("cores").value);
    var head = $("insns").tHead.rows[0];
    head.textContent = "";
    head.appendChild(el("th", { text: "Instruction" }));
    head.appendChild(el("th", { text: "Ext." }));
    cols.forEach(function (c) {
      head.appendChild(el("th", { cls: "num", text: colName(c, many) + " lat." }));
      head.appendChild(el("th", { cls: "num", text: colName(c, many) + " thr." }));
    });
    var ncols = 2 + 2 * cols.length;
    var order = [], seen = {}, byChip = {};
    chips.forEach(function (chip) {
      var idx = {};
      (loaded[chip.slug] ? loaded[chip.slug].instructions : []).forEach(function (ins) {
        idx[ins.name] = ins;
        if (!seen[ins.name]) { seen[ins.name] = true; order.push(ins); }
      });
      byChip[chip.slug] = idx;
    });
    var body = $("insns").tBodies[0];
    body.textContent = "";
    rows = [];
    var groups = [], sel = $("group"), keep = sel.value, last = null;
    sel.length = 1;
    order.forEach(function (ins) {
      if (ins.group === "Chain helpers") return;
      if (ins.group !== last) {
        last = ins.group;
        if (groups.indexOf(ins.group) < 0) groups.push(ins.group);
        var gh = el("tr", { cls: "grouphead" }, [el("td", { colspan: ncols, text: ins.group })]);
        body.appendChild(gh);
        rows.push({ head: true, group: ins.group, tr: gh });
      }
      var entries = {};
      chips.forEach(function (chip) { entries[chip.slug] = byChip[chip.slug][ins.name]; });
      var cells = [];
      cols.forEach(function (col) {
        var e = entries[col.chip.slug];
        cells.push(numCell(latSummary(e && e[col.core])));
        cells.push(numCell(tpSummary(e && e[col.core])));
      });
      var tr = el("tr", { cls: "row" }, [
        el("td", {}, [el("span", { cls: "asm", text: ins.asm })]),
        el("td", { cls: "dim", text: ins.ext || "" })
      ].concat(cells));
      tr.addEventListener("click", function () {
        toggleDetail(tr, function (td) { td.appendChild(insnDetail(ins, entries, cols)); }, ncols);
      });
      body.appendChild(tr);
      rows.push({ tr: tr, group: ins.group,
        hay: (ins.name + " " + ins.asm + " " + ins.group + " " + (ins.ext || "") + " " + (ins.note || "")).toLowerCase() });
    });
    groups.forEach(function (g) { sel.appendChild(el("option", { value: g, text: g })); });
    sel.value = groups.indexOf(keep) >= 0 ? keep : "";
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

  /* ---- datasets ------------------------------------------------------- */
  function sourceLink(chip, d) {
    var s = d.source || {};
    if (s.kind === "issue") return el("a", { href: REPO + "/issues/" + s.number, text: "issue #" + s.number });
    if (s.kind === "commit") return el("a", { href: REPO + "/commit/" + s.commit, text: "commit " + s.commit });
    if (s.kind === "pr") return el("a", { href: REPO + "/commits/main/" + d.file, text: "pull request (file history)" });
    return document.createTextNode("this computer, not submitted");
  }

  function renderDatasets() {
    var body = $("datasets").tBodies[0];
    body.textContent = "";
    chosen().forEach(function (chip) {
      chip.datasets.forEach(function (d) {
        var status = d.status + (d.compared ? ", " + d.marked + " of " + d.compared + " values marked" : ", nothing to compare with");
        var idCell = el("td", {}, [d.local ? el("code", { text: d.id }) : el("a", { href: REPO + "/blob/main/" + d.file, text: d.id })]);
        var tr = el("tr", { cls: d.marked ? "row" : "" }, [
          el("td", { text: chip.brand }), idCell, el("td", { text: d.model }), el("td", { text: d.os }),
          el("td", { text: d.tool }), el("td", { cls: "num", text: String(d.runs) }),
          el("td", { cls: "num", text: fmt(d.load[0]) + "–" + fmt(d.load[1]) }),
          el("td", { cls: "status-" + d.status, text: status }),
          el("td", {}, [sourceLink(chip, d)])
        ]);
        if (d.marked) {
          tr.addEventListener("click", function (ev) {
            if (ev.target.tagName === "A") return;
            toggleDetail(tr, function (td) {
              var ul = el("ul", { cls: "per" });
              d.marked_list.forEach(function (m) {
                ul.appendChild(el("li", {}, [el("code", { text: m.label }), ": " + fmt(m.value, 3) +
                  ", the other datasets " + fmt(m.center, 3) + " (" + fmt(m.lo, 3) + " to " + fmt(m.hi, 3) + ")"]));
              });
              if (d.marked > d.marked_list.length) ul.appendChild(el("li", { text: "and " + (d.marked - d.marked_list.length) + " more" }));
              td.appendChild(ul);
            }, 9);
          });
        }
        body.appendChild(tr);
      });
    });
  }

  /* ---- state ---------------------------------------------------------- */
  function selectionChanged() {
    try { history.replaceState(null, "", "#chips=" + selected.join(",")); } catch (e) { /* file:// */ }
    var chips = chosen();
    $("chipnote").textContent = chips.length > 1
      ? "Comparing " + chips.map(function (c) { return c.brand; }).join(", ") + "."
      : "Tick two or more chips to compare them side by side.";
    renderStructure();
    renderDatasets();
    var left = chips.length;
    $("count").textContent = "loading…";
    chips.forEach(function (chip) {
      loadChip(chip, function () { if (--left === 0) renderInsns(); });
    });
  }

  function init() {
    if (!INDEX.chips.length) {
      $("chipnote").textContent = "No results found: data.js is missing or empty (run make site).";
      return;
    }
    INDEX.chips.forEach(function (c) { bySlug[c.slug] = c; });
    if (INDEX.flag_share) $("flagshare").textContent = fmt(INDEX.flag_share * 100, 1) + " %";
    var m = /chips=([a-z0-9,-]+)/.exec(location.hash);
    if (m) selected = m[1].split(",").filter(function (s) { return bySlug[s]; });
    if (!selected.length) selected = [bySlug["apple-m5"] ? "apple-m5" : INDEX.chips[0].slug];
    renderChips();
    $("q").addEventListener("input", filter);
    $("group").addEventListener("change", filter);
    $("cores").addEventListener("change", renderInsns);
    selectionChanged();
  }
  init();
})();
