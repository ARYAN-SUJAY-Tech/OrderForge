'use strict';
(async () => {
  const { el, api, money, stockBadge, toast } = OF;
  const PER_PAGE = 12;
  const params = new URLSearchParams(location.search);
  const state = {
    q: params.get('q') || '',
    category: params.get('category') || '0',
    page: Math.max(1, parseInt(params.get('page') || '1', 10) || 1),
  };

  await OF.init('shop');

  const qInput = document.getElementById('q');
  const catSelect = document.getElementById('category');
  qInput.value = state.q;

  try {
    const { categories } = await api('GET', '/categories');
    for (const c of categories) {
      catSelect.append(el('option', { value: c.id }, `${c.name} (${c.products})`));
    }
    catSelect.value = state.category;
  } catch (e) { toast(e.message, 'error'); }

  document.getElementById('filters').addEventListener('submit', (ev) => {
    ev.preventDefault();
    state.q = qInput.value.trim();
    state.category = catSelect.value;
    state.page = 1;
    load();
  });
  catSelect.addEventListener('change', () => {
    state.category = catSelect.value;
    state.page = 1;
    load();
  });

  function card(p) {
    const add = el('button', { class: 'btn btn-sm', disabled: p.stock <= 0 }, 'Add to cart');
    add.addEventListener('click', async () => {
      if (!OF.requireLogin()) return;
      add.disabled = true;
      try {
        await OF.addToCart(p.id, 1);
        toast(`Added ${p.name} to your cart`, 'ok');
      } catch (e) { toast(e.message, 'error'); }
      add.disabled = p.stock <= 0;
    });
    return el('div', { class: 'card product' },
      el('a', { href: `/product.html?id=${p.id}`, class: 'thumb' }, p.name.charAt(0)),
      el('span', { class: 'cat' }, p.category),
      el('h3', {}, el('a', { href: `/product.html?id=${p.id}` }, p.name)),
      stockBadge(p.stock),
      el('div', { class: 'row' }, el('span', { class: 'price' }, money(p.price_paise)), add));
  }

  async function load() {
    const query = new URLSearchParams({ page: state.page, per_page: PER_PAGE });
    if (state.q) query.set('q', state.q);
    if (state.category !== '0') query.set('category', state.category);
    history.replaceState(null, '', '?' + query.toString());

    const grid = document.getElementById('products');
    try {
      const data = await api('GET', '/products?' + query.toString());
      grid.replaceChildren(...data.products.map(card));
      if (!data.products.length) grid.replaceChildren(el('p', { class: 'empty' }, 'No products match your search.'));
      document.getElementById('result-count').textContent =
        `${data.total} product${data.total === 1 ? '' : 's'}`;

      const pages = Math.max(1, Math.ceil(data.total / PER_PAGE));
      const prev = el('button', { class: 'btn btn-ghost', disabled: state.page <= 1 }, '← Previous');
      const next = el('button', { class: 'btn btn-ghost', disabled: state.page >= pages }, 'Next →');
      prev.addEventListener('click', () => { state.page--; load(); });
      next.addEventListener('click', () => { state.page++; load(); });
      document.getElementById('pager').replaceChildren(prev, el('span', { class: 'muted' }, `Page ${state.page} of ${pages}`), next);
    } catch (e) {
      toast(e.message, 'error');
    }
  }

  load();
})();
