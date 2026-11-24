'use strict';
(async () => {
  const { el, api, money, stockBadge, toast } = OF;
  await OF.init('shop');
  const root = document.getElementById('product');
  const id = parseInt(OF.qs('id'), 10);

  let p;
  try {
    p = (await api('GET', `/products/${id}`)).product;
  } catch (e) {
    root.replaceChildren(el('p', { class: 'empty' }, e.status === 404 ? 'Product not found.' : e.message));
    return;
  }
  document.title = `OrderForge – ${p.name}`;

  const qty = el('input', { type: 'number', min: 1, max: Math.max(1, Math.min(100, p.stock)), value: 1, 'aria-label': 'Quantity' });
  const add = el('button', { class: 'btn', disabled: p.stock <= 0 }, 'Add to cart');
  add.addEventListener('click', async () => {
    if (!OF.requireLogin()) return;
    const n = parseInt(qty.value, 10);
    if (!(n >= 1 && n <= 100)) { toast('Choose a quantity between 1 and 100', 'error'); return; }
    add.disabled = true;
    try {
      await OF.addToCart(p.id, n);
      toast(`Added ${n} × ${p.name} to your cart`, 'ok');
    } catch (e) { toast(e.message, 'error'); }
    add.disabled = false;
  });

  root.replaceChildren(el('div', { class: 'detail' },
    el('div', { class: 'card product' }, el('div', { class: 'thumb' }, p.name.charAt(0))),
    el('div', {},
      el('span', { class: 'cat' }, p.category),
      el('h1', {}, p.name),
      el('p', { class: 'muted mono' }, 'SKU ' + p.sku),
      el('div', { class: 'price' }, money(p.price_paise)),
      stockBadge(p.stock),
      el('p', {}, p.description),
      el('div', { class: 'qty-row' }, qty, add, el('a', { href: '/cart.html' }, 'View cart')))));
})();
