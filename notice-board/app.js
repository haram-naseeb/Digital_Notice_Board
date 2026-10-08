/* ================================================================
   NOTICE BOARD — Shared Utilities (app.js)
   Toast notifications + Confirm dialog + Auth system
   ================================================================ */

window.API_BASE = (window.NOTICE_BOARD_API_URL || (
  ['localhost', '127.0.0.1'].includes(window.location.hostname) ? 'http://localhost:3000' : ''
)).replace(/\/$/, '');

// Existing pages continue to use fetch(), while this adds the signed session
// token to API requests. Roles stored in localStorage remain presentation-only.
(() => {
  const nativeFetch = window.fetch.bind(window);
  window.fetch = async (input, init = {}) => {
    const url = typeof input === 'string' ? input : input.url;
    const apiOrigin = new URL(window.API_BASE || window.location.origin, window.location.origin).origin;
    const requestOrigin = new URL(url, window.location.origin).origin;
    if (requestOrigin === apiOrigin && !url.endsWith('/login')) {
      let user;
      try { user = JSON.parse(localStorage.getItem('nb_user') || 'null'); } catch { user = null; }
      if (user && user.token) {
        const headers = new Headers(init.headers || (input instanceof Request ? input.headers : undefined));
        headers.set('Authorization', `Bearer ${user.token}`);
        init = { ...init, headers };
      }
    }
    const response = await nativeFetch(input, init);
    if (response.status === 401 && !url.endsWith('/login')) {
      localStorage.removeItem('nb_user');
      window.location.href = 'login.html';
    }
    return response;
  };
})();

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
    // Strip emoji from messages for cleaner display
    const cleanMessage = message.replace(/[^\x00-\x7F]/g, '').trim() || message;
    toast.innerHTML = `
      <span class="toast-icon">${ICONS[type] || ICONS.success}</span>
      <span class="toast-message">${cleanMessage}</span>
      <button class="toast-close" aria-label="Dismiss">${CLOSE_ICON}</button>
    `;

    toast.querySelector('.toast-close').addEventListener('click', () => dismiss(toast));
    container.appendChild(toast);

    requestAnimationFrame(() => {
      requestAnimationFrame(() => toast.classList.add('toast-show'));
    });

    const timer = setTimeout(() => dismiss(toast), 4000);
    toast._dismissTimer = timer;
  };

  function dismiss(toast) {
    clearTimeout(toast._dismissTimer);
    toast.classList.remove('toast-show');
    setTimeout(() => { if (toast.parentElement) toast.remove(); }, 320);
  }
})();

/* ── Confirm Dialog ────────────────────────────────────────────── */
window.showConfirm = function (message, onConfirm, title = 'Confirm Action') {
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

/* ═══════════════════════════════════════════════════════════════
   AUTHENTICATION SYSTEM
   ═══════════════════════════════════════════════════════════════ */

/**
 * Check authentication status and enforce role-based access
 * @param {string} requiredRole - 'admin' for admin-only pages, null for any logged-in user
 * @returns {object} User object {username, role} if authenticated, else redirects to login
 */
window.checkAuth = function (requiredRole = null) {
  const userStr = localStorage.getItem('nb_user');

  if (!userStr) {
    window.location.href = 'login.html';
    return null;
  }

  try {
    const user = JSON.parse(userStr);

    if (!user.username || !user.role) {
      throw new Error('Invalid user object');
    }

    if (requiredRole && user.role !== requiredRole) {
      window.location.href = 'login.html';
      return null;
    }

    return user;
  } catch (e) {
    localStorage.removeItem('nb_user');
    window.location.href = 'login.html';
    return null;
  }
};

/** Logout the current user */
window.logout = function () {
  localStorage.removeItem('nb_user');
  window.location.href = 'home.html';
};

/** Get current user from localStorage */
window.getCurrentUser = function () {
  const userStr = localStorage.getItem('nb_user');
  if (!userStr) return null;
  try {
    return JSON.parse(userStr);
  } catch {
    return null;
  }
};

/**
 * Dynamically render the sidebar based on user role
 * @param {string} activeHref - The current page filename (e.g., 'index.html')
 */
window.renderSidebar = function (activeHref) {
  const user = getCurrentUser();
  if (!user) {
    window.location.href = 'login.html';
    return;
  }

  const isAdmin    = user.role === 'admin';
  const isChairman = user.role === 'chairman';

  const link = (href, icon, label) =>
    `<a href="${href}" ${activeHref === href ? 'class="active"' : ''}>
       <i data-lucide="${icon}"></i> ${label}
     </a>`;

  let navHtml = '';

  // Both admin and chairman can access the dashboard
  if (isAdmin || isChairman) {
    navHtml += link('index.html',         'layout-dashboard', 'Dashboard');
  }
  
  navHtml += link('announcements.html', 'megaphone',        'Announcements');

  if (isAdmin) {
    navHtml += link('timetable.html', 'calendar-days', 'Timetable');
    navHtml += `<div class="sidebar-section-label">Data Management</div>`;
    navHtml += link('sessions.html',  'graduation-cap', 'Sessions');
    navHtml += link('sections.html',  'users',          'Sections');
    navHtml += link('subjects.html',  'book-open',      'Subjects');
    navHtml += link('rooms.html',     'door-open',      'Rooms');
    navHtml += link('teachers.html',  'user-square',    'Teachers');
    navHtml += link('offices.html',   'building-2',     'Teacher Offices');
  }

  const roleBadgeStyle = isAdmin
    ? 'background:rgba(147,197,253,0.2);color:#93c5fd;border:1px solid rgba(147,197,253,0.3);'
    : 'background:rgba(252,211,77,0.2);color:#fcd34d;border:1px solid rgba(252,211,77,0.3);';

  const sidebarHtml = `
    <a class="sidebar-brand" href="index.html">
      <div class="sidebar-logos">
        <img src="uet_cs_logo.jpg" alt="CS">
      </div>
      <div>
        <span class="sidebar-brand-text">Notice Board</span>
        <span class="sidebar-brand-sub">Dept. of Computer Science</span>
      </div>
    </a>
    <div class="sidebar-user-info">
      <div class="sidebar-username">${user.username}</div>
      <span style="display:inline-block;padding:2px 9px;border-radius:20px;font-size:10px;font-weight:700;letter-spacing:0.4px;${roleBadgeStyle}">
        ${user.role.toUpperCase()}
      </span>
    </div>
    <div class="sidebar-nav">
      ${navHtml}
    </div>
    <button class="sidebar-logout" onclick="logout()">
      <i data-lucide="log-out"></i> Sign Out
    </button>
  `;

  const container = document.getElementById('sidebar-container');
  if (container) {
    container.innerHTML = sidebarHtml;
    container.className = 'sidebar';
    if (window.lucide) lucide.createIcons();
  }
};
