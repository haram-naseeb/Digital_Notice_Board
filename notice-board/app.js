/* ================================================================
   NOTICE BOARD — Shared Utilities (app.js)
   Toast notifications + Confirm dialog
   ================================================================ */

/* ── Toast System ─────────────────────────────────────────────── */
(function () {
  function getContainer() {
    let c = document.getElementById('toast-container');
    if (!c) {
      c = document.createElement('div');
      c.id = 'toast-container';
      document.body.appendChild(c);
    }
    return c;
  }

  const ICONS = {
    success: `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>`,
    error:   `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"></circle><line x1="15" y1="9" x2="9" y2="15"></line><line x1="9" y1="9" x2="15" y2="15"></line></svg>`,
    warning: `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path><line x1="12" y1="9" x2="12" y2="13"></line><line x1="12" y1="17" x2="12.01" y2="17"></line></svg>`,
  };

  const CLOSE_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;

  window.showToast = function (message, type = 'success') {
    const container = getContainer();
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.innerHTML = `
      <span class="toast-icon">${ICONS[type] || ICONS.success}</span>
      <span class="toast-message">${message}</span>
      <button class="toast-close" aria-label="Dismiss">${CLOSE_ICON}</button>
    `;

    // Close on button click
    toast.querySelector('.toast-close').addEventListener('click', () => dismiss(toast));

    container.appendChild(toast);

    // Trigger animation
    requestAnimationFrame(() => {
      requestAnimationFrame(() => toast.classList.add('toast-show'));
    });

    // Auto-dismiss after 3s
    const timer = setTimeout(() => dismiss(toast), 3000);
    toast._dismissTimer = timer;
  };

  function dismiss(toast) {
    clearTimeout(toast._dismissTimer);
    toast.classList.remove('toast-show');
    setTimeout(() => { if (toast.parentElement) toast.remove(); }, 320);
  }
})();

/* ── Confirm Dialog (replaces window.confirm) ─────────────────── */
window.showConfirm = function (message, onConfirm, title = 'Are you sure?') {
  // Remove any existing confirm
  const existing = document.getElementById('confirm-modal');
  if (existing) existing.remove();

  const WARN_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path><line x1="12" y1="9" x2="12" y2="13"></line><line x1="12" y1="17" x2="12.01" y2="17"></line></svg>`;

  const overlay = document.createElement('div');
  overlay.id = 'confirm-modal';
  overlay.className = 'confirm-overlay';
  overlay.innerHTML = `
    <div class="confirm-box">
      <div class="confirm-icon">${WARN_ICON}</div>
      <p class="confirm-title">${title}</p>
      <p class="confirm-message">${message}</p>
      <div class="confirm-actions">
        <button class="btn btn-secondary" id="confirm-cancel-btn">Cancel</button>
        <button class="btn btn-danger-fill" id="confirm-ok-btn">Confirm</button>
      </div>
    </div>
  `;

  document.body.appendChild(overlay);

  // Show animation
  requestAnimationFrame(() => {
    requestAnimationFrame(() => overlay.classList.add('confirm-show'));
  });

  function close() {
    overlay.classList.remove('confirm-show');
    setTimeout(() => { if (overlay.parentElement) overlay.remove(); }, 220);
  }

  overlay.addEventListener('click', (e) => { if (e.target === overlay) close(); });
  document.getElementById('confirm-cancel-btn').addEventListener('click', close);
  document.getElementById('confirm-ok-btn').addEventListener('click', () => {
    close();
    onConfirm();
  });
};

/* ── Lucide icon init helper ──────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
  if (window.lucide) lucide.createIcons();
});
