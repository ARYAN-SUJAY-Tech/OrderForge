'use strict';
(async () => {
  const session = await OF.init('login');
  const next = OF.safeNext(OF.qs('next'));
  if (session) { location.href = next; return; }

  function wire(formId, errId, path, fields) {
    const form = document.getElementById(formId);
    const err = document.getElementById(errId);
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      err.textContent = '';
      const body = {};
      for (const f of fields) body[f] = form.elements[f].value.trim();
      body.password = form.elements.password.value; // never trim passwords
      const btn = form.querySelector('button');
      btn.disabled = true;
      try {
        OF.setSession(await OF.api('POST', path, body));
        location.href = next;
      } catch (e) {
        err.textContent = e.message;
        btn.disabled = false;
      }
    });
  }

  wire('login-form', 'login-error', '/auth/login', ['email']);
  wire('register-form', 'register-error', '/auth/register', ['name', 'email']);
})();
