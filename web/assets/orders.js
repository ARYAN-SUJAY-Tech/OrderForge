'use strict';
// orders.html        : the customer's order history
// orders.html?id=N   : one order with its items and tracking timeline (admins: any order)
(async () => {
  const { el, api, money, fmtDate, statusBadge, toast } = OF;
  await OF.init('orders');
  if (!OF.requireLogin()) return;
  const root = document.getElementById('orders');
  const id = parseInt(OF.qs('id'), 10);

  if (id > 0) {
    await showOrder(id);
    // Status changes (payment, packing, shipping) arrive while the page is open: poll.
    setInterval(() => showOrder(id, true), 5000);
  } else {
    await showList();
  }

  async function showList() {
    let data;
    try { data = await api('GET', '/orders'); } catch (e) { toast(e.message, 'error'); return; }
    if (!data.orders.length) {
      root.replaceChildren(el('h1', {}, 'Your orders'), el('div', { class: 'card empty' },
        el('p', {}, 'You have not placed any orders yet.'), el('a', { href: '/', class: 'btn' }, 'Start shopping')));
      return;
    }
    root.replaceChildren(el('h1', {}, 'Your orders'), el('div', { class: 'table-wrap' }, el('table', {},
      el('thead', {}, el('tr', {}, el('th', {}, 'Order'), el('th', {}, 'Placed'), el('th', {}, 'Status'),
        el('th', { class: 'num' }, 'Items'), el('th', { class: 'num' }, 'Total'))),
      el('tbody', {}, data.orders.map((o) => el('tr', {},
        el('td', {}, el('a', { href: `/orders.html?id=${o.id}` }, `#${o.id}`)),
        el('td', {}, fmtDate(o.created_at)),
        el('td', {}, statusBadge(o.status)),
        el('td', { class: 'num' }, o.units),
        el('td', { class: 'num' }, money(o.total_paise))))))));
  }

  async function showOrder(orderId, quiet) {
    let o;
    try { o = (await api('GET', `/orders/${orderId}`)).order; } catch (e) {
      if (!quiet) root.replaceChildren(el('p', { class: 'empty' }, e.status === 404 ? 'Order not found.' : e.message));
      return;
    }
    const placed = OF.qs('placed') === '1';
    root.replaceChildren(
      el('p', {}, el('a', { href: OF.isAdmin() ? '/admin/orders.html' : '/orders.html' }, '← All orders')),
      placed ? el('div', { class: 'card' }, el('strong', {}, 'Thank you! Your order has been placed.')) : null,
      el('h1', {}, `Order #${o.id} `, statusBadge(o.status)),
      el('p', { class: 'muted' }, `Placed ${fmtDate(o.created_at)}`, OF.isAdmin() ? ` by ${o.email}` : ''),
      el('div', { class: 'table-wrap' }, el('table', {},
        el('thead', {}, el('tr', {}, el('th', {}, 'Product'), el('th', { class: 'num' }, 'Unit price'),
          el('th', { class: 'num' }, 'Qty'), el('th', { class: 'num' }, 'Total'))),
        el('tbody', {}, o.items.map((i) => el('tr', {},
          el('td', {}, i.name, el('div', { class: 'muted mono' }, i.sku)),
          el('td', { class: 'num' }, money(i.unit_price_paise)),
          el('td', { class: 'num' }, i.qty),
          el('td', { class: 'num' }, money(i.unit_price_paise * i.qty))))))),
      el('div', { class: 'summary' }, el('span', {}, 'Order total ', el('strong', {}, money(o.total_paise)))),
      el('h2', {}, 'Tracking'),
      el('ol', { class: 'timeline' }, o.events.map((ev) => el('li', {},
        el('div', {}, el('strong', {}, ev.to.replace('_', ' ')), ev.note ? ` – ${ev.note}` : ''),
        el('div', { class: 'when' }, `${fmtDate(ev.at)} · ${ev.actor}`)))));
  }
})();
