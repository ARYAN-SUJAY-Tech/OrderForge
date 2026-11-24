// OrderForge shared front-end helpers. Plain JS, no build step.
// All text is inserted with textContent (via el()), never innerHTML, so product names
// and other user data cannot inject markup.
'use strict';

const OF = (() => {
  let session = null; // { user: {id,email,name,role}, csrf_token }

  async function api(method, path, body) {
    const opts = { method, headers: {}, credentials: 'same-origin' };
    if (body !== undefined) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    if (method !== 'GET' && session) opts.headers['X-CSRF-Token'] = session.csrf_token;
    const res = await fetch('/api' + path, opts);
    let data = null;
    try { data = await res.json(); } catch (_) { /* empty or non-JSON body */ }
    if (!res.ok) {
      const err = new Error((data && data.error && data.error.message) || `HTTP ${res.status}`);
      err.status = res.status;
      err.code = data && data.error && data.error.code;
      throw err;
    }
    return data;
  }

  async function loadSession() {
    try { session = await api('GET', '/me'); } catch (_) { session = null; }
    return session;
  }

  function setSession(s) { session = s; }
  function user() { return session && session.user; }
  function isAdmin() { return !!(session && session.user.role === 'admin'); }

  // el('a', {href: '/x', class: 'btn', onclick: fn}, 'text', childNode, ...)
  function el(tag, attrs, ...children) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
      else if (k === 'class') node.className = v;
      else if (k === 'dataset') Object.assign(node.dataset, v);
      else if (v === true) node.setAttribute(k, '');
      else node.setAttribute(k, String(v));
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      node.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return node;
  }

  function money(paise) {
    return '₹' + (paise / 100).toLocaleString('en-IN', {
      minimumFractionDigits: 2, maximumFractionDigits: 2,
    });
  }

  function fmtDate(iso) {
    if (!iso) return '';
    const d = new Date(iso);
    return isNaN(d) ? iso : d.toLocaleString('en-IN', { dateStyle: 'medium', timeStyle: 'short' });
  }

  function qs(name) { return new URLSearchParams(location.search).get(name); }

  function statusBadge(status) {
    return el('span', { class: 'badge status-' + status }, status.replace('_', ' '));
  }

  function stockBadge(stock) {
    if (stock <= 0) return el('span', { class: 'badge stock-out' }, 'Out of stock');
    if (stock <= 10) return el('span', { class: 'badge stock-low' }, `Only ${stock} left`);
    return el('span', { class: 'badge stock-ok' }, 'In stock');
  }

  let toastTimer;
  function toast(msg, kind = 'info') {
    let t = document.getElementById('toast');
    if (!t) {
      t = el('div', { id: 'toast', role: 'status', 'aria-live': 'polite' });
      document.body.append(t);
    }
    t.textContent = msg;
    t.className = 'show ' + kind;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.className = ''; }, 3500);
  }

  // Only same-site relative paths are allowed as a post-login redirect.
  function safeNext(next) {
    return next && next.startsWith('/') && !next.startsWith('//') ? next : '/';
  }

  function requireLogin() {
    if (!session) {
      location.href = '/login.html?next=' + encodeURIComponent(location.pathname + location.search);
      return false;
    }
    return true;
  }

  async function cartCount() {
    if (!session) return 0;
    try { return (await api('GET', '/cart')).units; } catch (_) { return 0; }
  }

  async function addToCart(productId, n) {
    const cart = await api('GET', '/cart');
    const line = cart.items.find((i) => i.product_id === productId);
    const qty = Math.min(100, (line ? line.qty : 0) + n);
    const res = await api('POST', '/cart/items', { product_id: productId, qty });
    renderCartCount(res.units);
    return res;
  }

  function renderCartCount(n) {
    const b = document.getElementById('cart-count');
    if (b) { b.textContent = n; b.hidden = !n; }
  }

  async function logout() {
    try { await api('POST', '/auth/logout'); } catch (_) { /* already gone */ }
    session = null;
    location.href = '/';
  }

  // Render the site header into <header id="site-header"> and load the session.
  async function init(active) {
    await loadSession();
    const h = document.getElementById('site-header');
    if (!h) return session;
    const link = (href, label, key) =>
      el('a', { href, class: active === key ? 'active' : null }, label);

    const nav = el('nav', { class: 'nav' },
      link('/', 'Shop', 'shop'),
      session ? el('a', { href: '/cart.html', class: active === 'cart' ? 'active' : null },
        'Cart ', el('span', { id: 'cart-count', class: 'count', hidden: true }, '0')) : null,
      session ? link('/orders.html', 'Orders', 'orders') : null,
      isAdmin() ? link('/admin/', 'Admin', 'admin') : null,
    );
    const right = el('div', { class: 'who' },
      session
        ? [el('span', { class: 'muted' }, session.user.name),
           el('button', { class: 'btn btn-ghost', onclick: logout }, 'Log out')]
        : el('a', { href: '/login.html', class: 'btn' }, 'Log in'));

    h.replaceChildren(el('div', { class: 'bar' },
      el('a', { href: '/', class: 'brand' }, el('span', { class: 'logo' }, 'OF'), 'OrderForge'),
      nav, right));

    if (session) cartCount().then(renderCartCount);
    return session;
  }

  return {
    api, init, loadSession, setSession, user, isAdmin, el, money, fmtDate, qs,
    statusBadge, stockBadge, toast, safeNext, requireLogin, addToCart, renderCartCount,
  };
})();
