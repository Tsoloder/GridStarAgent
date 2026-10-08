"use strict";

// 轻量 SVG 图表：折线、饼、柱。零外部依赖，配色全部走 CSS 变量，三套主题自动适配。
// 时间轴缩放锚定鼠标位置；折线与柱状共享同一视窗，视窗由调用方持有并下发。
const Charts = (function () {
  const NS = "http://www.w3.org/2000/svg";
  const PAD = {left: 62, right: 16, top: 14, bottom: 30};
  const SERIES_COLORS = ["var(--cyan)", "var(--green)", "var(--orange)"];
  const PIE_COLORS = ["var(--cyan)", "var(--green)", "var(--orange)", "var(--assistant-text)",
    "var(--cyan-mid)", "var(--red)", "var(--muted-2)"];
  const MIN_ZOOM_RATIO = 0.01;

  function svgEl(name, attrs) {
    const node = document.createElementNS(NS, name);
    for (const key in attrs) node.setAttribute(key, attrs[key]);
    return node;
  }

  function escapeText(value) {
    return String(value == null ? "" : value).replace(/[&<>"']/g,
      char => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;"})[char]);
  }

  // 桶标签解析成本地时间。手写而不用 Date.parse：无时区的 "YYYY-MM-DDTHH:MM"
  // 在旧浏览器上解释不一致，这里显式按本地时间构造。
  function parseBucket(label) {
    const text = String(label == null ? "" : label);
    const year = parseInt(text.slice(0, 4), 10);
    const month = parseInt(text.slice(5, 7), 10);
    const day = parseInt(text.slice(8, 10), 10);
    if (isNaN(year) || isNaN(month) || isNaN(day)) return 0;
    const hour = text.length > 10 ? parseInt(text.slice(11, 13), 10) : 0;
    return new Date(year, month - 1, day, isNaN(hour) ? 0 : hour, 0, 0, 0).getTime();
  }

  const MILLION = 1e6;

  // 用量一律以百万（M）为单位：同一页混用 K 与 M，数值之间难以横向比较，
  // 所以不足 1M 也写成小数（如 0.5M），不退回 K。
  function formatMillions(value) {
    const tokens = Number(value) || 0;
    if (tokens === 0) return "0M";
    const millions = tokens / MILLION;
    const abs = Math.abs(millions);
    // 1M 以上留两位小数；不足 1M 时按数量级补足小数位，
    // 否则固定两位会把几千的用量压成 0.00M，看起来像没有消耗
    const digits = abs >= 1 ? 2 : Math.min(8, 1 - Math.floor(Math.log(abs) / Math.LN10));
    let text = millions.toFixed(digits);
    if (text.indexOf(".") >= 0) text = text.replace(/0+$/, "").replace(/\.$/, "");
    return `${text}M`;
  }

  // 坐标轴刻度与数值同单位；0 只写 0，不写 0M
  function axisLabel(value) {
    const tokens = Number(value) || 0;
    return tokens === 0 ? "0" : formatMillions(tokens);
  }

  function clamp(value, low, high) {
    return value < low ? low : (value > high ? high : value);
  }

  function pad2(value) {
    return String(value).padStart(2, "0");
  }

  function hasData(buckets) {
    return buckets.some(row => row.total > 0);
  }

  function domainOfBuckets(buckets) {
    if (!buckets.length) {
      const now = Date.now();
      return [now - 3600000, now];
    }
    const start = parseBucket(buckets[0].t);
    let end = parseBucket(buckets[buckets.length - 1].t);
    if (end <= start) end = start + 3600000;
    return [start, end];
  }

  // 视窗只能落在给定区间内部，且不允许缩到比最小跨度还窄
  function clampView(view, domain) {
    const low = domain[0], high = domain[1];
    if (!view) return {start: low, end: high};
    const minSpan = Math.max((high - low) * MIN_ZOOM_RATIO, 1000);
    let start = clamp(view.start, low, high);
    let end = clamp(view.end, low, high);
    if (end - start < minSpan) {
      const middle = (start + end) / 2;
      start = clamp(middle - minSpan / 2, low, Math.max(low, high - minSpan));
      end = start + minSpan;
    }
    return {start: start, end: end};
  }

  function niceStep(raw) {
    if (!(raw > 0)) return 1;
    const magnitude = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10));
    const normalized = raw / magnitude;
    const factor = normalized <= 1 ? 1 : (normalized <= 2 ? 2 : (normalized <= 5 ? 5 : 10));
    return factor * magnitude;
  }

  function yScale(max, plotH, tickCount) {
    const step = niceStep(max / tickCount);
    const top = Math.max(step, Math.ceil(max / step) * step);
    const ticks = [];
    for (let value = 0; value <= top + step / 2; value += step) ticks.push(Math.round(value));
    if (ticks[ticks.length - 1] !== top) ticks.push(top);
    return {
      top: top,
      ticks: ticks,
      y: value => PAD.top + plotH - (top > 0 ? clamp(value / top, 0, 1) * plotH : 0),
    };
  }

  function xLabel(ms, span) {
    const date = new Date(ms);
    const monthDay = `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
    if (span > 3 * 86400000) return monthDay;
    if (span > 6 * 3600000) return `${monthDay} ${pad2(date.getHours())}:00`;
    return `${pad2(date.getHours())}:00`;
  }

  function bucketTitle(label) {
    const date = new Date(parseBucket(label));
    return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())} ${pad2(date.getHours())}:00`;
  }

  // 每个容器一份跨次渲染保留的状态：纵轴缩放倍数、被隐藏的序列
  function nodeState(container) {
    if (!container.__chartState) container.__chartState = {yZoom: 1, hidden: {}};
    return container.__chartState;
  }

  function frame(container, height, className) {
    container.innerHTML = "";
    const width = Math.max(280, container.clientWidth || 0);
    const svg = svgEl("svg", {
      width: "100%", height: height, viewBox: `0 0 ${width} ${height}`,
      role: "img", class: className || "",
    });
    svg.style.width = "100%";
    svg.style.height = `${height}px`;
    container.append(svg);
    return {
      svg: svg, width: width,
      plotW: Math.max(40, width - PAD.left - PAD.right),
      plotH: Math.max(40, height - PAD.top - PAD.bottom),
    };
  }

  function legendRow(container, items, hidden, onToggle) {
    const row = document.createElement("div");
    row.className = "chart-legend";
    items.forEach((item, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = hidden[item.key] ? "off" : "";
      button.setAttribute("aria-pressed", String(!hidden[item.key]));
      button.innerHTML = `<i style="background:${item.color || SERIES_COLORS[index % SERIES_COLORS.length]}"></i>${escapeText(item.label)}`;
      button.onclick = () => onToggle(item.key);
      row.append(button);
    });
    container.insertBefore(row, container.firstChild);
    return row;
  }

  function emptyNote(container, text) {
    const note = document.createElement("div");
    note.className = "chart-empty";
    note.textContent = text;
    container.append(note);
  }

  function tooltip(container) {
    const tip = document.createElement("div");
    tip.className = "chart-tip hidden";
    container.append(tip);
    return tip;
  }

  function clipGroup(ctx, suffix) {
    const id = `chart-clip-${suffix}-${Math.random().toString(36).slice(2, 9)}`;
    const defs = svgEl("defs", {});
    const clip = svgEl("clipPath", {id: id});
    clip.append(svgEl("rect", {x: PAD.left, y: PAD.top - 4, width: ctx.plotW, height: ctx.plotH + 8}));
    defs.append(clip);
    ctx.svg.append(defs);
    const group = svgEl("g", {"clip-path": `url(#${id})`});
    ctx.svg.append(group);
    return {group: group, defs: defs};
  }

  // 网格、纵轴刻度、横轴刻度；返回时间到像素的映射
  function drawAxes(ctx, view, scale) {
    const span = Math.max(1, view.end - view.start);
    const x = ms => PAD.left + ((ms - view.start) / span) * ctx.plotW;
    ctx.svg.append(svgEl("rect", {
      x: PAD.left, y: PAD.top, width: ctx.plotW, height: ctx.plotH,
      fill: "none", stroke: "var(--line)", "stroke-width": 1,
    }));
    scale.ticks.forEach(value => {
      const y = scale.y(value);
      ctx.svg.append(svgEl("line", {
        x1: PAD.left, y1: y, x2: PAD.left + ctx.plotW, y2: y,
        stroke: "var(--line)", "stroke-width": 1, "stroke-dasharray": value ? "3 4" : "",
      }));
      const label = svgEl("text", {
        x: PAD.left - 8, y: y + 3, "text-anchor": "end",
        fill: "var(--muted-2)", "font-size": 9,
      });
      label.textContent = axisLabel(value);
      ctx.svg.append(label);
    });
    const tickCount = Math.max(2, Math.min(7, Math.floor(ctx.plotW / 110)));
    let previousLabel = "";
    for (let index = 0; index <= tickCount; index += 1) {
      const ms = view.start + (span * index) / tickCount;
      const text = xLabel(ms, span);
      // 相邻刻度可能落在同一天，重复的同名刻度不画
      if (text === previousLabel) continue;
      previousLabel = text;
      const label = svgEl("text", {
        x: clamp(x(ms), PAD.left + 14, PAD.left + ctx.plotW - 14), y: PAD.top + ctx.plotH + 16,
        "text-anchor": "middle", fill: "var(--muted-2)", "font-size": 9,
      });
      label.textContent = text;
      ctx.svg.append(label);
    }
    return x;
  }

  // 视窗内的桶，首尾各多带一个邻居，让线段延伸到视窗之外由裁剪收边
  function visibleSlice(buckets, view) {
    const inside = [];
    buckets.forEach((row, index) => {
      const ms = parseBucket(row.t);
      if (ms >= view.start && ms <= view.end) inside.push(index);
    });
    if (!inside.length) return [];
    const from = Math.max(0, inside[0] - 1);
    const to = Math.min(buckets.length - 1, inside[inside.length - 1] + 1);
    return buckets.slice(from, to + 1).map(row => ({ms: parseBucket(row.t), row: row}));
  }

  function nearestBucket(buckets, target) {
    let best = null, bestGap = Infinity;
    buckets.forEach(row => {
      const gap = Math.abs(parseBucket(row.t) - target);
      if (gap < bestGap) { bestGap = gap; best = row; }
    });
    return best;
  }

  function ratioAt(ctx, event) {
    const rect = ctx.svg.getBoundingClientRect();
    return clamp((event.clientX - rect.left - PAD.left) / Math.max(1, rect.width - PAD.left - PAD.right), 0, 1);
  }

  // 滚轮缩放（锚定鼠标位置）、拖拽平移、双击复位；Shift + 滚轮缩放纵轴
  function bindTimeZoom(container, domain, view, onChange, onYChange) {
    const svg = container.querySelector("svg");
    if (!svg) return;
    const emit = next => { if (onChange) onChange(next); };
    svg.addEventListener("wheel", event => {
      const magnitude = event.deltaY * (event.deltaMode === 1 ? 16 : (event.deltaMode === 2 ? 100 : 1));
      if (event.shiftKey) {
        event.preventDefault();
        if (onYChange) onYChange(Math.exp(magnitude * 0.002));
        return;
      }
      event.preventDefault();
      const rect = svg.getBoundingClientRect();
      const ratio = clamp((event.clientX - rect.left - PAD.left) / Math.max(1, rect.width - PAD.left - PAD.right), 0, 1);
      const anchor = view.start + (view.end - view.start) * ratio;
      const factor = Math.exp(magnitude * 0.002);
      emit(clampView({
        start: anchor - (anchor - view.start) * factor,
        end: anchor + (view.end - anchor) * factor,
      }, domain));
    }, {passive: false});

    svg.addEventListener("mousedown", event => {
      if (event.button !== 0) return;
      event.preventDefault();
      const rect = container.getBoundingClientRect();
      const plotW = Math.max(1, rect.width - PAD.left - PAD.right);
      const span = view.end - view.start;
      const originX = event.clientX;
      const origin = {start: view.start, end: view.end};
      const move = moveEvent => {
        const shift = ((moveEvent.clientX - originX) / plotW) * span;
        // 平移钳制的是偏移量而不是两端：独立钳制两端会在贴边时压扁视窗，把缩放层级弄丢
        const start = clamp(origin.start - shift, domain[0], Math.max(domain[0], domain[1] - span));
        emit({start: start, end: start + span});
      };
      const stop = () => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", stop);
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", stop);
    });

    svg.addEventListener("dblclick", () => emit(null));
    svg.style.cursor = "grab";
  }

  function line(container, options) {
    const buckets = options.buckets || [];
    const series = options.series || [];
    const domain = options.domain || domainOfBuckets(buckets);
    const node = nodeState(container);
    const height = options.height || 230;
    const view = clampView(options.view, domain);
    const shown = series.filter(item => !node.hidden[item.key]);

    if (!buckets.length || !hasData(buckets)) {
      container.innerHTML = '<div class="chart-empty">所选范围内没有用量记录</div>';
      return;
    }

    const ctx = frame(container, height, "chart-svg");
    legendRow(container, series, node.hidden, key => {
      node.hidden[key] = !node.hidden[key];
      line(container, options);
    });
    if (!shown.length) {
      emptyNote(container, "所有序列均已隐藏，点击图例恢复");
      return;
    }

    let peak = 0;
    shown.forEach(item => buckets.forEach(row => {
      const value = Number(row[item.key]) || 0;
      if (value > peak) peak = value;
    }));
    const scale = yScale(peak * node.yZoom, ctx.plotH, 4);
    const x = drawAxes(ctx, view, scale);
    const clipped = clipGroup(ctx, "line");
    const slice = visibleSlice(buckets, view);

    series.forEach((item, index) => {
      if (node.hidden[item.key]) return;
      const color = item.color || SERIES_COLORS[index % SERIES_COLORS.length];
      const points = slice.map(entry => `${x(entry.ms)},${scale.y(Number(entry.row[item.key]) || 0)}`);
      if (points.length > 1) {
        clipped.group.append(svgEl("polyline", {
          points: points.join(" "), fill: "none", stroke: color,
          "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round",
        }));
      }
      slice.forEach(entry => {
        clipped.group.append(svgEl("circle", {
          cx: x(entry.ms), cy: scale.y(Number(entry.row[item.key]) || 0), r: 2.5,
          fill: color, "fill-opacity": 0.9,
        }));
      });
    });

    const guide = svgEl("line", {
      x1: PAD.left, y1: PAD.top, x2: PAD.left, y2: PAD.top + ctx.plotH,
      stroke: "var(--cyan)", "stroke-width": 1, "stroke-dasharray": "3 3",
      opacity: 0, "pointer-events": "none",
    });
    ctx.svg.append(guide);
    const tip = tooltip(container);
    const overlay = svgEl("rect", {
      x: PAD.left, y: PAD.top, width: ctx.plotW, height: ctx.plotH, fill: "transparent",
    });
    ctx.svg.append(overlay);
    overlay.addEventListener("mousemove", event => {
      const target = view.start + (view.end - view.start) * ratioAt(ctx, event);
      const row = nearestBucket(buckets, target);
      if (!row) return;
      const px = x(parseBucket(row.t));
      guide.setAttribute("x1", px);
      guide.setAttribute("x2", px);
      guide.setAttribute("opacity", 1);
      tip.innerHTML = `<strong>${escapeText(bucketTitle(row.t))}</strong>`
        + `<span>总用量 <b>${formatMillions(row.total)}</b></span>`
        + `<span>输入 <b>${formatMillions(row.input)}</b></span>`
        + `<span>输出 <b>${formatMillions(row.output)}</b></span>`;
      tip.classList.remove("hidden");
      const width = ctx.svg.getBoundingClientRect().width;
      tip.style.left = `${clamp(px - 90, 4, Math.max(4, width - 184))}px`;
      if (options.onHover) options.onHover(row);
    });
    overlay.addEventListener("mouseleave", () => {
      guide.setAttribute("opacity", 0);
      tip.classList.add("hidden");
      if (options.onHover) options.onHover(null);
    });

    bindTimeZoom(container, domain, view, options.onViewChange, factor => {
      node.yZoom = clamp(node.yZoom * factor, 0.05, 20);
      line(container, options);
    });

    const rows = buckets.filter(row => {
      const ms = parseBucket(row.t);
      return ms >= view.start && ms <= view.end;
    });
    const summary = shown.map(item => {
      const sum = rows.reduce((total, row) => total + (Number(row[item.key]) || 0), 0);
      return `${item.label} ${formatMillions(sum)}`;
    });
    ctx.svg.setAttribute("aria-label", `${rows.length} 个时间点的用量趋势：${summary.join("，")}`);
  }

  function bar(container, options) {
    const buckets = options.buckets || [];
    const domain = options.domain || domainOfBuckets(buckets);
    const node = nodeState(container);
    const height = options.height || 190;
    const view = clampView(options.view, domain);
    const hidden = node.hidden;

    if (!buckets.length || !hasData(buckets)) {
      container.innerHTML = '<div class="chart-empty">所选范围内没有用量记录</div>';
      return;
    }

    const ctx = frame(container, height, "chart-svg");
    legendRow(container, [
      {key: "measured", label: "实测", color: "var(--cyan)"},
      {key: "estimated", label: "估算", color: "var(--orange)"},
    ], hidden, key => {
      hidden[key] = !hidden[key];
      bar(container, options);
    });

    const peak = buckets.reduce((best, row) => {
      let sum = 0;
      if (!hidden.measured) sum += Number(row.measured) || 0;
      if (!hidden.estimated) sum += Number(row.estimated) || 0;
      return Math.max(best, sum);
    }, 0);
    const scale = yScale(peak * node.yZoom, ctx.plotH, 4);
    const x = drawAxes(ctx, view, scale);
    const clipped = clipGroup(ctx, "bar");

    // 估算段用斜纹区分，不靠颜色单一通道传达实测与估算的差别
    const pattern = svgEl("pattern", {
      id: "chart-bar-stripe", width: 5, height: 5,
      patternUnits: "userSpaceOnUse", patternTransform: "rotate(45)",
    });
    pattern.append(svgEl("rect", {width: 5, height: 5, fill: "var(--orange)", "fill-opacity": 0.16}));
    pattern.append(svgEl("line", {x1: 0, y1: 0, x2: 0, y2: 5, stroke: "var(--orange)", "stroke-width": 2}));
    clipped.defs.append(pattern);

    const slot = ctx.plotW / Math.max(1, buckets.length);
    const barWidth = Math.max(1, Math.min(26, slot * 0.66));
    buckets.forEach(row => {
      const ms = parseBucket(row.t);
      if (ms < view.start || ms > view.end) return;
      const left = clamp(x(ms) - barWidth / 2, PAD.left, PAD.left + ctx.plotW - barWidth);
      let base = PAD.top + ctx.plotH;
      [["measured", "var(--cyan)", ""], ["estimated", "var(--orange)", "url(#chart-bar-stripe)"]]
        .forEach(pair => {
          if (hidden[pair[0]]) return;
          const value = Number(row[pair[0]]) || 0;
          if (value <= 0) return;
          const heightPx = Math.max(1, base - scale.y(value));
          clipped.group.append(svgEl("rect", {
            x: left, y: base - heightPx, width: barWidth, height: heightPx,
            fill: pair[2] || pair[1], stroke: pair[1], "stroke-width": pair[2] ? 0.6 : 0,
          }));
          base -= heightPx;
        });
    });

    const guide = svgEl("line", {
      x1: PAD.left, y1: PAD.top, x2: PAD.left, y2: PAD.top + ctx.plotH,
      stroke: "var(--cyan)", "stroke-width": 1, "stroke-dasharray": "3 3",
      opacity: 0, "pointer-events": "none",
    });
    ctx.svg.append(guide);
    const tip = tooltip(container);
    const overlay = svgEl("rect", {
      x: PAD.left, y: PAD.top, width: ctx.plotW, height: ctx.plotH, fill: "transparent",
    });
    ctx.svg.append(overlay);
    overlay.addEventListener("mousemove", event => {
      const target = view.start + (view.end - view.start) * ratioAt(ctx, event);
      const row = nearestBucket(buckets, target);
      if (!row) return;
      const px = x(parseBucket(row.t));
      guide.setAttribute("x1", px);
      guide.setAttribute("x2", px);
      guide.setAttribute("opacity", 1);
      tip.innerHTML = `<strong>${escapeText(bucketTitle(row.t))}</strong>`
        + `<span>实测 <b>${formatMillions(row.measured)}</b></span>`
        + `<span>估算 <b>${formatMillions(row.estimated)}</b></span>`
        + `<span>合计 <b>${formatMillions(row.total)}</b></span>`;
      tip.classList.remove("hidden");
      const width = ctx.svg.getBoundingClientRect().width;
      tip.style.left = `${clamp(px - 90, 4, Math.max(4, width - 184))}px`;
      if (options.onHover) options.onHover(row);
    });
    overlay.addEventListener("mouseleave", () => {
      guide.setAttribute("opacity", 0);
      tip.classList.add("hidden");
      if (options.onHover) options.onHover(null);
    });

    bindTimeZoom(container, domain, view, options.onViewChange, factor => {
      node.yZoom = clamp(node.yZoom * factor, 0.05, 20);
      bar(container, options);
    });

    const rows = buckets.filter(row => {
      const ms = parseBucket(row.t);
      return ms >= view.start && ms <= view.end;
    });
    const measured = rows.reduce((total, row) => total + row.measured, 0);
    const estimated = rows.reduce((total, row) => total + row.estimated, 0);
    ctx.svg.setAttribute("aria-label",
      `${rows.length} 个时间点的用量构成：实测 ${formatMillions(measured)}，估算 ${formatMillions(estimated)}`);
  }

  function arcPath(cx, cy, outer, inner, startAngle, endAngle) {
    const large = endAngle - startAngle > Math.PI ? 1 : 0;
    const x1 = cx + outer * Math.cos(startAngle), y1 = cy + outer * Math.sin(startAngle);
    const x2 = cx + outer * Math.cos(endAngle), y2 = cy + outer * Math.sin(endAngle);
    const x3 = cx + inner * Math.cos(endAngle), y3 = cy + inner * Math.sin(endAngle);
    const x4 = cx + inner * Math.cos(startAngle), y4 = cy + inner * Math.sin(startAngle);
    return ["M", x1, y1, "A", outer, outer, 0, large, 1, x2, y2,
      "L", x3, y3, "A", inner, inner, 0, large, 0, x4, y4, "Z"].join(" ");
  }

  function pie(container, options) {
    const groups = (options.groups || []).filter(item => item.value > 0);
    const total = groups.reduce((sum, item) => sum + item.value, 0);
    if (!total) {
      container.innerHTML = '<div class="chart-empty">所选范围内没有用量记录</div>';
      return;
    }

    // 图例固定占右侧，先落进 DOM 再量画布宽度，避免按整幅宽度算圆心
    container.innerHTML = "";
    const split = document.createElement("div");
    split.className = "pie-split";
    const wrap = document.createElement("div");
    wrap.className = "pie-legend";
    split.append(wrap);
    const holder = document.createElement("div");
    holder.className = "pie-canvas";
    split.append(holder);
    container.append(split);

    const ctx = frame(holder, options.height || 190, "chart-svg");
    const center = {x: PAD.left + ctx.plotW / 2, y: PAD.top + ctx.plotH / 2};
    const outer = Math.max(26, Math.min(ctx.plotH / 2, ctx.plotW * 0.44));
    const inner = outer * 0.6;

    const sectors = [];
    let angle = -Math.PI / 2;
    groups.forEach((item, index) => {
      const sweep = (item.value / total) * Math.PI * 2;
      const path = svgEl("path", {
        d: arcPath(center.x, center.y, outer, inner, angle, angle + Math.max(sweep - 0.004, 0.0001)),
        fill: PIE_COLORS[index % PIE_COLORS.length], stroke: "var(--panel)", "stroke-width": 1,
      });
      ctx.svg.append(path);
      sectors.push({item: item, path: path, color: PIE_COLORS[index % PIE_COLORS.length]});
      angle += sweep;
    });

    const centerValue = svgEl("text", {
      x: center.x, y: center.y - 2, "text-anchor": "middle",
      fill: "var(--text-bright)", "font-size": 15, "font-weight": 600,
    });
    const centerLabel = svgEl("text", {
      x: center.x, y: center.y + 13, "text-anchor": "middle",
      fill: "var(--muted-2)", "font-size": 9,
    });
    ctx.svg.append(centerValue);
    ctx.svg.append(centerLabel);
    ctx.svg.setAttribute("aria-label",
      `用量占比：${groups.map(item => `${item.label} ${Math.round(item.value / total * 100)}%`).join("，")}`);

    const focus = index => {
      sectors.forEach((sector, position) => {
        sector.path.setAttribute("fill-opacity", index === null || position === index ? 1 : 0.35);
      });
      const current = index === null ? null : sectors[index];
      centerValue.textContent = formatMillions(current ? current.item.value : total);
      centerLabel.textContent = current
        ? `${current.item.label} · ${Math.round(current.item.value / total * 100)}%`
        : (options.unit || "总量");
    };
    focus(null);

    sectors.forEach((sector, index) => {
      sector.path.addEventListener("mouseenter", () => focus(index));
      sector.path.addEventListener("mouseleave", () => focus(null));
    });

    groups.forEach((item, index) => {
      const row = document.createElement("div");
      row.className = "pie-legend-row";
      row.innerHTML = `<i style="background:${sectors[index].color}"></i>`
        + `<span class="pie-legend-name" title="${escapeText(item.label)}">${escapeText(item.label)}</span>`
        + `<span class="pie-legend-pct">${(item.value / total * 100).toFixed(1)}%</span>`
        + `<b>${formatMillions(item.value)}</b>`;
      row.addEventListener("mouseenter", () => focus(index));
      row.addEventListener("mouseleave", () => focus(null));
      wrap.append(row);
    });
    ctx.svg.setAttribute("role", "img");
  }

  return {line: line, pie: pie, bar: bar, parseBucket: parseBucket, formatMillions: formatMillions};
})();