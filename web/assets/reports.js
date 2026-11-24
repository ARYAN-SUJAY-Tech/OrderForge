'use strict';
// Admin reports dashboard. Sales figures come from of-report's aggregate tables; order
// and payment counts, low stock and system health are live.
(async () => {
  const { el, api, money, toast } = OF;
  await OF.init('admin');
  if (!OF.requireLogin()) return;
  const main = document.querySelector('main');
  if (!OF.isAdmin()) {
    main.replaceChildren(el('p', { class: 'empty' }, 'Admin access required.'));
    return;
  }
  main.prepend(OF.adminNav('reports'));

  const daysSel = document.getElementById('days');
  const SVG = 'http://www.w3.org/2000/svg';
  const svg = (tag, attrs) => {
    const n = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs || {})) n.setAttribute(k, String(v));
    return n;
  };

  // ₹ in compact form for axis ticks: ₹950, ₹12k, ₹3.4L (lakh), ₹1.2Cr (crore).
  function compact(paise) {
    const r = paise / 100;
    if (r >= 1e7) return `₹${(r / 1e7).toFixed(1)}Cr`;
    if (r >= 1e5) return `₹${(r / 1e5).toFixed(1)}L`;
    if (r >= 1e3) return `₹${(r / 1e3).toFixed(r >= 1e4 ? 0 : 1)}k`;
    return `₹${Math.round(r)}`;
  }

  // A "nice" axis maximum and step (1, 2, 2.5 or 5 x 10^n) for about 4 gridlines.
  function niceScale(max) {
    if (max <= 0) return { top: 100000, step: 25000 };
    const raw = max / 4;
    const pow = 10 ** Math.floor(Math.log10(raw));
    const step = [1, 2, 2.5, 5, 10].map((m) => m * pow).find((s) => s >= raw);
    return { top: Math.ceil(max / step) * step, step };
  }

  // Every day in the window, zero-filled, so gaps in sales show as gaps in the chart.
  function fillDays(daily, days) {
    const byDay = new Map(daily.map((d) => [d.day, d]));
    const out = [];
    const today = new Date();
    for (let i = days - 1; i >= 0; i--) {
      const d = new Date(Date.UTC(today.getUTCFullYear(), today.getUTCMonth(), today.getUTCDate() - i));
      const key = d.toISOString().slice(0, 10);
      out.push(byDay.get(key) || { day: key, orders: 0, units: 0, revenue_paise: 0 });
    }
    return out;
  }

  const dayLabel = (iso) => new Date(iso + 'T00:00:00Z')
    .toLocaleDateString('en-IN', { day: 'numeric', month: 'short', timeZone: 'UTC' });

  function drawChart(series) {
    const box = document.getElementById('chart');
    const W = 880, H = 240, L = 56, R = 8, T = 12, B = 28;
    const plotW = W - L - R, plotH = H - T - B;
    const { top, step } = niceScale(Math.max(...series.map((d) => d.revenue_paise)));
    const y = (v) => T + plotH - (v / top) * plotH;
    const slot = plotW / series.length;
    const gap = 2; // surface gap between adjacent bars
    const barW = Math.max(1, slot - gap);

    const root = svg('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img',
      'aria-label': 'Daily revenue bar chart; the same data is available as a table below' });

    // Recessive grid and y-axis labels.
    for (let v = 0; v <= top + 1e-9; v += step) {
      root.append(svg('line', { x1: L, x2: W - R, y1: y(v), y2: y(v), class: v === 0 ? 'axis' : 'grid' }));
      const t = svg('text', { x: L - 8, y: y(v) + 4, 'text-anchor': 'end', class: 'tick' });
      t.textContent = compact(v);
      root.append(t);
    }

    // x labels: about 7 evenly spaced dates.
    const every = Math.max(1, Math.ceil(series.length / 7));
    series.forEach((d, i) => {
      if (i % every !== 0 && i !== series.length - 1) return;
      const t = svg('text', { x: L + i * slot + slot / 2, y: H - 8, 'text-anchor': 'middle', class: 'tick' });
      t.textContent = dayLabel(d.day);
      root.append(t);
    });

    // Bars: 4 px rounded top, square at the baseline.
    const tip = el('div', { class: 'chart-tip', role: 'tooltip', hidden: true });
    series.forEach((d, i) => {
      const x = L + i * slot + gap / 2;
      if (d.revenue_paise > 0) {
        const h = Math.max(2, (d.revenue_paise / top) * plotH);
        const r = Math.min(4, barW / 2, h);
        const yTop = T + plotH - h, yBase = T + plotH;
        root.append(svg('path', {
          class: 'bar',
          d: `M${x},${yBase} V${yTop + r} Q${x},${yTop} ${x + r},${yTop} H${x + barW - r} ` +
             `Q${x + barW},${yTop} ${x + barW},${yTop + r} V${yBase} Z`,
        }));
      }
      // Hit target: the whole column, wider than the bar.
      const hit = svg('rect', { x: L + i * slot, y: T, width: slot, height: plotH, class: 'hit' });
      hit.addEventListener('mouseenter', () => {
        tip.replaceChildren(
          el('strong', {}, dayLabel(d.day)),
          el('div', {}, `Revenue ${money(d.revenue_paise)}`),
          el('div', { class: 'muted' }, `${d.orders} order${d.orders === 1 ? '' : 's'} · ${d.units} unit${d.units === 1 ? '' : 's'}`));
        tip.hidden = false;
        const pct = (L + i * slot + slot / 2) / W;
        tip.style.left = `${pct * 100}%`;
        hit.classList.add('on');
      });
      hit.addEventListener('mouseleave', () => { tip.hidden = true; hit.classList.remove('on'); });
      root.append(hit);
    });

    box.replaceChildren(root, tip);
  }

  function table(id, head, rows, empty) {
    const t = document.getElementById(id);
    t.replaceChildren(
      el('thead', {}, el('tr', {}, head.map(([label, num]) => el('th', { class: num ? 'num' : null }, label)))),
      el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', { colspan: head.length, class: 'empty' }, empty))));
  }

  function chips(id, counts, order) {
    const keys = order.filter((k) => counts[k]);
    document.getElementById(id).replaceChildren(...(keys.length ? keys.map((k) =>
      el('span', { class: 'chip' }, OF.statusBadge(k), el('strong', {}, counts[k])))
      : [el('span', { class: 'muted' }, 'None yet.')]));
  }

  async function load() {
    const days = parseInt(daysSel.value, 10);
    let d;
    try { d = await api('GET', `/admin/reports?days=${days}`); } catch (e) { toast(e.message, 'error'); return; }

    document.getElementById('generated').textContent = d.generated_at
      ? `Sales aggregated by of-report ${OF.fmtDate(d.generated_at)} (${d.age_seconds}s ago, took ${d.generation_ms} ms)`
      : 'No sales report yet: is of-report running?';

    const avg = d.totals.orders ? Math.round(d.totals.revenue_paise / d.totals.orders) : 0;
    document.getElementById('kpis').replaceChildren(
      ...[['Revenue', money(d.totals.revenue_paise)], ['Paid orders', d.totals.orders],
        ['Units sold', d.totals.units], ['Average order', money(avg)]].map(([label, value]) =>
        el('div', { class: 'card kpi' }, el('div', { class: 'muted' }, label), el('div', { class: 'kpi-value' }, value))));

    const series = fillDays(d.daily, days);
    document.getElementById('chart-sub').textContent =
      `Revenue from paid, packed, shipped and delivered orders, per day (UTC), last ${days} days`;
    drawChart(series);
    table('daily-table', [['Day'], ['Orders', 1], ['Units', 1], ['Revenue', 1]],
      d.daily.map((r) => el('tr', {}, el('td', {}, r.day), el('td', { class: 'num' }, r.orders),
        el('td', { class: 'num' }, r.units), el('td', { class: 'num' }, money(r.revenue_paise)))),
      'No sales in this window.');

    table('top', [['#'], ['Product'], ['Units', 1], ['Revenue', 1]],
      d.top_products.map((p, i) => el('tr', {}, el('td', {}, i + 1),
        el('td', {}, p.name, el('div', { class: 'muted mono' }, p.sku)),
        el('td', { class: 'num' }, p.units), el('td', { class: 'num' }, money(p.revenue_paise)))),
      'No sales in this window.');

    table('low', [['Product'], ['Stock', 1], ['Reorder at', 1]],
      d.low_stock.map((p) => el('tr', {},
        el('td', {}, p.name, el('div', { class: 'muted mono' }, p.sku)),
        el('td', { class: 'num' }, OF.stockBadge(p.stock), ' ', p.stock),
        el('td', { class: 'num' }, p.reorder_level))),
      'Everything is above its reorder level.');

    chips('status-counts', d.order_status,
      ['placed', 'payment_pending', 'paid', 'packed', 'shipped', 'delivered', 'cancelled', 'refunded']);
    chips('payment-counts', d.payments,
      ['pending', 'succeeded', 'declined', 'timed_out', 'cancelled', 'refunded']);
  }

  async function health() {
    let s;
    try { s = await api('GET', '/admin/system'); } catch (_) { return; }
    const m = s.metrics || {};
    const rows = [
      ['Stock mode', s.stock_mode], ['Reservation TTL', `${s.reservation_ttl_s} s`],
      ['Products in shared memory', s.products_in_shm], ['Requests', m.requests_total],
      ['Orders placed', m.orders_total], ['Rejected (sold out)', m.orders_failed],
      ['Busy rejections', m.busy_rejects], ['Queue depth', m.queue_depth],
      ['Active workers', m.active_workers], ['CAS retries', m.cas_retries],
      ['Payments sent', m.payments_sent], ['Approved', m.payments_succeeded],
      ['Declined', m.payments_declined], ['Duplicates ignored', m.payments_ignored],
      ['Forged/replayed rejected', m.payments_rejected], ['Late payments refunded', m.late_refunds],
      ['Reservations expired', m.reservations_expired],
    ];
    document.getElementById('health').replaceChildren(...rows.flatMap(([k, v]) =>
      [el('dt', {}, k), el('dd', {}, v === undefined ? '–' : String(v))]));
  }

  for (const b of document.querySelectorAll('[data-export]')) {
    b.addEventListener('click', async () => {
      try {
        const r = await api('GET', `/admin/reports/export?name=${b.dataset.export}`);
        OF.download(r.filename, r.csv);
      } catch (e) { toast(e.message, 'error'); }
    });
  }
  daysSel.addEventListener('change', load);
  load();
  health();
  setInterval(health, 2000);
  setInterval(load, 15000);
})();
