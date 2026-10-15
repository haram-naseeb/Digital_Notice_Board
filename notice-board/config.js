/*
 * Local development uses the Express server that serves this page. The Vercel
 * deployment uses the public Render API. This lets contributors run `npm start`
 * and open localhost:3000 without editing or committing this file.
 */
const isLocalDevelopment = ['localhost', '127.0.0.1'].includes(window.location.hostname);
window.NOTICE_BOARD_API_URL = isLocalDevelopment
  ? ''
  : 'https://knotify-api-bv4i.onrender.com';
