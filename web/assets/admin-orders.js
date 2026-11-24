'use strict';
(async () => {
  const { el, api, money, fmtDate, statusBadge, toast } = OF;
  await OF.init('admin');
  if (!OF.requireLogin()) return;
  const main = document.querySelector('main');
  if (!OF.isAdmin()) {
    main.replaceChildren(el('p', { class: 'empty' }, 'Admin access required.'));
    return;
  }
  main.prepend(OF.adminNav('orders'));

  const PER_PAGE = 25;
  const statusSel = document.getElementById('status');
  let page = 1;

  async function move(o, to, label) {
    if ((to === 'refunded' || to === 'cancelled') && !confirm(`${label} order #${o.id}?`)) return;
    try {
      await api('POST', `/admin/orders/${o.id}/status`, { status: to });
      toast(`Order #${o.id}: ${to}`, 'ok');
    } catch (e) { toast(e.message, 'error'); }
    load();
  }

  function actions(o) {
    return el('div', { class: 'actions' }, OF.adminNext(o.status).map(([to, label]) => {
      const b = el('button', { class: to === 'refunded' || to === 'cancelled' ? 'btn btn-danger btn-sm' : 'btn btn-sm' }, label);
      b.addEventListener('click', () => move(o, to, label));
      return b;
    }));
  }

  async function load() {
    const q = new URLSearchParams({ page, per_page: PER_PAGE });
    if (statusSel.value) q.set('status', statusSel.value);
    let data;
    try { data = await api('GET', '/admin/orders?' + q); } catch (e) { toast(e.message, 'error'); return; }

    document.getElementById('count').textContent = `${data.total} order${data.total === 1 ? '' : 's'}`;
    const rows = data.orders.map((o) => el('tr', {},
      el('td', {}, el('a', { href: `/orders.html?id=${o.id}` }, `#${o.id}`)),
      el('td', {}, o.email),
      el('td', {}, fmtDate(o.created_at)),
      el('td', {}, statusBadge(o.status)),
      el('td', { class: 'num' }, o.units),
      el('td', { class: 'num' }, money(o.total_paise)),
      el('td', {}, actions(o))));
    document.getElementById('rows').replaceChildren(...rows);
    if (!rows.length) {
      document.getElementById('rows').replaceChildren(el('tr', {}, el('td', { colspan: 7, class: 'empty' }, 'No orders.')));
    }

    const pages = Math.max(1, Math.ceil(data.total / PER_PAGE));
    const prev = el('button', { class: 'btn btn-ghost', disabled: page <= 1 }, '← Previous');
    const next = el('button', { class: 'btn btn-ghost', disabled: page >= pages }, 'Next →');
    prev.addEventListener('click', () => { page--; load(); });
    next.addEventListener('click', () => { page++; load(); });
    document.getElementById('pager').replaceChildren(prev, el('span', { class: 'muted' }, `Page ${page} of ${pages}`), next);
  }

  statusSel.addEventListener('change', () => { page = 1; load(); });
  load();
  setInterval(load, 5000); // the board updates as orders come in
})();
