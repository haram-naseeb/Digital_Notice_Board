# KNOTIFY deployment guide

This project can run locally or with a Vercel frontend, Render Node service, and
Aiven MySQL. It deliberately keeps the current public HiveMQ connection:
`broker.hivemq.com:1883`, topic `department/notices`, QoS 1, and the existing
payload and retained-message behavior.

## 1. Prepare the database

Create a free Aiven MySQL service. Import the existing `digital_notice_board`
schema and data from the local database with MySQL Workbench (Server > Data
Export, then Data Import/Restore). No schema migration is required.

Copy the Aiven host, port, database, user, password, and CA certificate from
its connection information. Aiven requires TLS. The server has
`rejectUnauthorized: true`; never disable certificate validation. If you use a
CA value in an environment variable, replace each line break with `\n`.

## 2. Deploy the backend on Render

Create a new **Web Service** from this repository. Use Node, leave the build
command as `npm install`, and set the start command to `npm start`. Render
provides `PORT` automatically.

Set these environment variables in Render:

| Variable | Value |
| --- | --- |
| `NODE_ENV` | `production` |
| `JWT_SECRET` | a new long random secret, unique to this deployment |
| `AUTH_TOKEN_TTL_HOURS` | `8` (or your desired session duration) |
| `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD`, `DB_NAME` | Aiven connection values |
| `DB_SSL` | `true` |
| `DB_SSL_CA` | Aiven CA, with line breaks represented as `\n` when needed |
| `ENABLE_MDNS` | `false` |
| `ALLOWED_ORIGINS` | the Vercel site URL, for example `https://your-project.vercel.app` |

After deployment, open `https://YOUR-RENDER-SERVICE.onrender.com/health`. It
returns `200` with `{"status":"ok"...}` only when MySQL is reachable; it
returns `503` when the service is alive but the database is unavailable.

`/diagnostics` now requires an authenticated Admin token and should not be used
as a public status page. Free Render services may sleep during inactivity, so
the first request after a quiet period can take longer.

## 3. Deploy the frontend on Vercel

Before deploying, edit the one non-secret setting in `config.js`:

```js
window.NOTICE_BOARD_API_URL = 'https://YOUR-RENDER-SERVICE.onrender.com';
```

Commit that URL change, then import the repository into Vercel as a static
site. No build command is needed. Once Vercel provides its final URL, add it to
Render's `ALLOWED_ORIGINS` and redeploy the backend. If Vercel gives both a
production domain and preview domains, list each required exact origin,
comma-separated.

For local development, leave `config.js` blank. The pages automatically use
`http://localhost:3000` when opened on localhost.

## 4. Local development

1. Copy `.env.example` to `.env` without committing it, then fill in the local
   MySQL password. The server loads this file only for local development.
2. Set `ENABLE_MDNS=true` only if `noticeboard.local` discovery is wanted on
   the local network.
3. Run `npm install`, then `npm start`.
4. Open `http://localhost:3000/home.html`, sign in, and verify
   `http://localhost:3000/health`.

Render supplies its own environment variables; it does not need a `.env` file.

## Authentication and roles

Successful login now returns a signed, time-limited access token. The browser
sends it automatically with API calls. Local storage still keeps the username,
role, and token to preserve the existing user experience, but it is no longer
trusted for authorization. The backend enforces these permissions:

| Action | Role |
| --- | --- |
| Create or delete announcements | Admin |
| Approve announcements | Chairman |
| Timetable, sessions, sections, subjects, rooms, teachers, offices, test clear, and MQTT timetable sync | Admin |

Existing public read endpoints stay available for the installed ESP32 firmware.

## ESP32 and MQTT limitations

No MQTT broker, topic, client, QoS, retention setting, payload format, or
firmware MQTT setting was changed. The public HiveMQ broker has no
authentication or TLS, which means topic traffic is not private and another
user could publish to the same public topic. It also uses one retained value
per topic. A local and cloud backend running at the same time share the fixed
server MQTT client ID and should be avoided.

The existing firmware's HTTP integration still points to `192.168.0.112:3000`
over plain HTTP. It will continue to work against a local server. To make its
HTTP timetable/announcement/office reads use Render, it must be deliberately
updated to use HTTPS with `WiFiClientSecure`, certificate validation using the
Render certificate chain/root CA, and the Render hostname. This deployment
preparation does not make that firmware change, because it would alter the
current working device network behavior. MQTT continues working independently
with its present HiveMQ settings.

## Operational behavior

The server checks expiry once at startup and every 30 seconds thereafter. A
lock prevents overlapping expiry runs during a slow or temporarily unavailable
database. MySQL uses a connection pool and reports failed keep-alive queries
without terminating the service. MQTT reconnection remains handled by the
existing MQTT client configuration.
