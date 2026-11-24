'use strict';
(async () => {
  const { el, api, money, toast } = OF;
  await OF.init('cart');
  if (!OF.requireLogin()) return;
  const root = document.getElementById('cart');
  let checkoutKey = OF.newKey(); // Idempotency-Key for this checkout attempt

  function render(cart) {
    OF.renderCartCount(cart.units);
    if (!cart.items.length) {
      root.replaceChildren(el('div', { class: 'card empty' },
        el('p', {}, 'Your cart is empty.'), el('a', { href: '/', class: 'btn' }, 'Browse products')));
      return;
    }

    const rows = cart.items.map((i) => {
      const qty = el('input', { type: 'number', class: 'cart-qty', min: 0, max: 100, value: i.qty, 'aria-label': `Quantity of ${i.name}` });
      qty.addEventListener('change', () => update(i.product_id, parseInt(qty.value, 10)));
      const remove = el('button', { class: 'btn btn-ghost btn-sm' }, 'Remove');
      remove.addEventListener('click', () => update(i.product_id, 0));
      const warn = !i.active ? 'No longer available'
        : i.stock < i.qty ? `Only ${i.stock} in stock` : null;
      return el('tr', {},
        el('td', {}, el('a', { href: `/product.html?id=${i.product_id}` }, i.name),
          el('div', { class: 'muted mono' }, i.sku), warn ? el('div', { class: 'error' }, warn) : null),
        el('td', { class: 'num' }, money(i.price_paise)),
        el('td', { class: 'num' }, qty),
        el('td', { class: 'num' }, money(i.line_total_paise)),
        el('td', {}, remove));
    });

    // Mock payment: the token decides what the gateway does. No card data exists.
    const method = el('select', { id: 'pay-method', 'aria-label': 'Payment method' },
      el('option', { value: 'tok_success' }, 'Test card: always approved'),
      el('option', { value: 'tok_decline' }, 'Test card: always declined'),
      el('option', { value: 'tok_timeout' }, 'Test card: gateway never answers'),
      el('option', { value: 'tok_duplicate' }, 'Test card: approved, reply sent twice'),
      el('option', { value: 'tok_random' }, 'Test card: random outcome'));

    const checkout = el('button', { class: 'btn' }, 'Place order');
    checkout.addEventListener('click', async () => {
      checkout.disabled = true;
      try {
        const { order } = await api('POST', '/checkout', { payment_method: method.value },
          { 'Idempotency-Key': checkoutKey });
        checkoutKey = OF.newKey(); // the next order is a new attempt
        location.href = `/orders.html?id=${order.id}&placed=1`;
      } catch (e) {
        // A network failure keeps the same key, so a retry can never create a second
        // order; an answer from the server (e.g. out of stock) starts a new attempt.
        if (e.status) checkoutKey = OF.newKey();
        toast(e.message, 'error');
        checkout.disabled = false;
        load();
      }
    });

    root.replaceChildren(
      el('div', { class: 'table-wrap' }, el('table', {},
        el('thead', {}, el('tr', {},
          el('th', {}, 'Product'), el('th', { class: 'num' }, 'Price'), el('th', { class: 'num' }, 'Qty'),
          el('th', { class: 'num' }, 'Total'), el('th', {}, ''))),
        el('tbody', {}, rows))),
      el('div', { class: 'summary' },
        el('span', { class: 'muted' }, `${cart.units} item${cart.units === 1 ? '' : 's'}`),
        el('span', {}, 'Total ', el('strong', {}, money(cart.total_paise)))),
      el('div', { class: 'summary' },
        el('label', { class: 'inline' }, 'Pay with', method),
        checkout),
      el('p', { class: 'muted right' },
        'Payments go to a mock gateway. Choose a test card to try each outcome.'));
  }

  async function update(productId, qty) {
    if (!(qty >= 0 && qty <= 100)) { toast('Quantity must be 0 to 100', 'error'); return; }
    try { render(await api('POST', '/cart/items', { product_id: productId, qty })); }
    catch (e) { toast(e.message, 'error'); load(); }
  }

  async function load() {
    try { render(await api('GET', '/cart')); } catch (e) { toast(e.message, 'error'); }
  }
  load();
})();
