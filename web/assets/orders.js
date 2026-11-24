'use strict';
// orders.html        : the customer's order history
// orders.html?id=N   : one order with its payment, tracking timeline and actions
//                      (admins can open any order and move it through fulfilment)
(async () => {
  const { el, api, money, fmtDate, statusBadge, toast } = OF;
  await OF.init('orders');
  if (!OF.requireLogin()) return;
  const root = document.getElementById('orders');
  const id = parseInt(OF.qs('id'), 10);

  if (id > 0) {
    // Poll every 2 s while the order can still change (payment, packing, shipping).
    let timer = null;
    const refresh = async () => {
      const o = await showOrder(id);
      if (o && OF.isFinal(o.status) && timer) { clearInterval(timer); timer = null; }
    };
    await refresh();
    timer = setInterval(refresh, 2000);
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

  function paymentLine(o) {
    const p = o.payment;
    if (!p) return null;
    const text = {
      pending: 'Waiting for the payment gateway…',
      succeeded: 'Payment approved',
      declined: 'Payment declined: nothing was charged',
      timed_out: 'The payment gateway did not answer in time: nothing was charged',
      cancelled: 'Payment cancelled: nothing was charged',
      refunded: 'Payment refunded',
    }[p.status] || p.status;
    return el('div', { class: 'card payment payment-' + p.status },
      el('strong', {}, text),
      el('div', { class: 'muted' }, `${money(p.amount_paise)} · ${p.method}`,
        p.provider_ref ? ` · ref ${p.provider_ref}` : ''),
      p.note ? el('div', { class: 'muted' }, p.note) : null,
      o.status === 'payment_pending' && o.reservation_seconds_left !== undefined
        ? el('div', { class: 'muted' }, `Your items are held for ${o.reservation_seconds_left} more seconds.`)
        : null);
  }

  async function act(orderId, label, fn) {
    try {
      await fn();
      toast(label, 'ok');
      await showOrder(orderId);
    } catch (e) { toast(e.message, 'error'); }
  }

  function actions(o) {
    const buttons = [];
    if (OF.isAdmin()) {
      for (const [to, label] of OF.adminNext(o.status)) {
        const b = el('button', { class: to === 'refunded' || to === 'cancelled' ? 'btn btn-danger' : 'btn' }, label);
        b.addEventListener('click', () => {
          if ((to === 'refunded' || to === 'cancelled') && !confirm(`${label} order #${o.id}?`)) return;
          act(o.id, `Order #${o.id}: ${to}`, () => api('POST', `/admin/orders/${o.id}/status`, { status: to }));
        });
        buttons.push(b);
      }
    } else if (['placed', 'payment_pending', 'paid'].includes(o.status)) {
      const label = o.status === 'paid' ? 'Cancel and refund' : 'Cancel order';
      const b = el('button', { class: 'btn btn-danger' }, label);
      b.addEventListener('click', () => {
        if (!confirm(`${label}?`)) return;
        act(o.id, 'Order cancelled', () => api('POST', `/orders/${o.id}/cancel`));
      });
      buttons.push(b);
    }
    return buttons.length ? el('div', { class: 'actions' }, buttons) : null;
  }

  async function showOrder(orderId) {
    let o;
    try { o = (await api('GET', `/orders/${orderId}`)).order; } catch (e) {
      root.replaceChildren(el('p', { class: 'empty' }, e.status === 404 ? 'Order not found.' : e.message));
      return null;
    }
    const placed = OF.qs('placed') === '1';
    // (replaceChildren would print a null as the text "null", so drop the absent parts)
    root.replaceChildren(...[
      el('p', {}, el('a', { href: OF.isAdmin() ? '/admin/orders.html' : '/orders.html' }, '← All orders')),
      placed ? el('div', { class: 'card' }, el('strong', {}, 'Thank you! Your order has been placed.')) : null,
      el('div', { class: 'title-row' },
        el('h1', {}, `Order #${o.id} `, statusBadge(o.status)), actions(o)),
      el('p', { class: 'muted' }, `Placed ${fmtDate(o.created_at)}`, OF.isAdmin() ? ` by ${o.email}` : ''),
      paymentLine(o),
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
        el('div', { class: 'when' }, `${fmtDate(ev.at)} · ${ev.actor}`))))].filter(Boolean));
    return o;
  }
})();
