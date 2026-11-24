'use strict';
(async () => {
  const { el, api, money, toast } = OF;
  await OF.init('admin');
  if (!OF.requireLogin()) return;
  if (!OF.isAdmin()) {
    document.querySelector('main').replaceChildren(el('p', { class: 'empty' }, 'Admin access required.'));
    return;
  }

  const form = document.getElementById('editor');
  const errBox = document.getElementById('editor-error');
  const lowOnly = document.getElementById('low-only');
  let editing = null; // product being edited, or null when adding

  const { categories } = await api('GET', '/categories');
  for (const c of categories) form.elements.category_id.append(el('option', { value: c.id }, c.name));

  // Rupees in the form, integer paise on the wire.
  const toPaise = (rupees) => Math.round(parseFloat(rupees) * 100);

  function startEdit(p) {
    editing = p;
    document.getElementById('editor-title').textContent = `Edit ${p.sku}`;
    document.getElementById('editor-submit').textContent = 'Save changes';
    document.getElementById('editor-cancel').hidden = false;
    document.getElementById('stock-field').hidden = true;
    form.elements.sku.value = p.sku;
    form.elements.sku.disabled = true;
    form.elements.name.value = p.name;
    form.elements.price.value = (p.price_paise / 100).toFixed(2);
    form.elements.category_id.value = p.category_id;
    form.elements.reorder_level.value = p.reorder_level;
    form.elements.description.value = p.description;
    form.scrollIntoView({ behavior: 'smooth' });
  }

  function resetEditor() {
    editing = null;
    form.reset();
    form.elements.sku.disabled = false;
    document.getElementById('editor-title').textContent = 'Add a product';
    document.getElementById('editor-submit').textContent = 'Add product';
    document.getElementById('editor-cancel').hidden = true;
    document.getElementById('stock-field').hidden = false;
    errBox.textContent = '';
  }
  document.getElementById('editor-cancel').addEventListener('click', resetEditor);

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    errBox.textContent = '';
    const f = form.elements;
    const body = {
      name: f.name.value.trim(),
      description: f.description.value.trim(),
      price_paise: toPaise(f.price.value),
      category_id: parseInt(f.category_id.value, 10),
      reorder_level: parseInt(f.reorder_level.value, 10) || 0,
    };
    try {
      if (editing) {
        await api('PUT', `/admin/products/${editing.id}`, body);
        toast(`Saved ${editing.sku}`, 'ok');
      } else {
        body.sku = f.sku.value.trim().toUpperCase();
        body.stock = parseInt(f.stock.value, 10) || 0;
        await api('POST', '/admin/products', body);
        toast(`Added ${body.sku}`, 'ok');
      }
      resetEditor();
      load();
    } catch (e) { errBox.textContent = e.message; }
  });

  async function adjustStock(p, deltaInput, reasonSelect) {
    const delta = parseInt(deltaInput.value, 10);
    if (!delta) { toast('Enter a non-zero amount', 'error'); return; }
    try {
      await api('POST', `/admin/products/${p.id}/stock`, { delta, reason: reasonSelect.value });
      toast(`${p.sku}: stock ${delta > 0 ? '+' : ''}${delta}`, 'ok');
      load();
    } catch (e) { toast(e.message, 'error'); }
  }

  async function setActive(p, active) {
    try {
      if (active) await api('PUT', `/admin/products/${p.id}`, { active: true });
      else await api('DELETE', `/admin/products/${p.id}`);
      toast(`${p.sku} ${active ? 'restored' : 'removed from the shop'}`, 'ok');
      load();
    } catch (e) { toast(e.message, 'error'); }
  }

  function row(p) {
    const delta = el('input', { type: 'number', class: 'cart-qty', placeholder: '+/-', 'aria-label': `Stock change for ${p.sku}` });
    const reason = el('select', { 'aria-label': 'Reason' },
      el('option', { value: 'restock' }, 'Restock'), el('option', { value: 'adjust' }, 'Adjust'));
    const apply = el('button', { class: 'btn btn-ghost btn-sm' }, 'Apply');
    apply.addEventListener('click', () => adjustStock(p, delta, reason));

    const edit = el('button', { class: 'btn btn-ghost btn-sm' }, 'Edit');
    edit.addEventListener('click', () => startEdit(p));
    const toggle = el('button', { class: p.active ? 'btn btn-danger btn-sm' : 'btn btn-sm' }, p.active ? 'Delete' : 'Restore');
    toggle.addEventListener('click', () => setActive(p, !p.active));

    const low = p.stock <= p.reorder_level;
    return el('tr', { class: p.active ? null : 'inactive' },
      el('td', {}, p.id),
      el('td', { class: 'mono' }, p.sku),
      el('td', {}, p.name, p.active ? null : el('div', { class: 'muted' }, 'deleted')),
      el('td', {}, p.category),
      el('td', { class: 'num' }, money(p.price_paise)),
      el('td', { class: 'num' }, low ? el('span', { class: 'badge stock-low' }, p.stock) : p.stock,
        el('div', { class: 'muted' }, `reorder at ${p.reorder_level}`)),
      el('td', {}, el('div', { class: 'actions' }, delta, reason, apply)),
      el('td', {}, el('div', { class: 'actions' }, edit, toggle)));
  }

  async function load() {
    try {
      const { products } = await api('GET', '/admin/products' + (lowOnly.checked ? '?low_stock=1' : ''));
      document.getElementById('rows').replaceChildren(...products.map(row));
    } catch (e) { toast(e.message, 'error'); }
  }
  lowOnly.addEventListener('change', load);
  load();
})();
