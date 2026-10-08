/**
 * Password Reset Utility
 * Run: node reset-passwords.js
 * 
 * This will update the admin and chairman passwords in MySQL.
 * Default credentials after running:
 *   admin     → admin123
 *   chairman  → chairman123
 */

require('dotenv').config();

const mysql = require('mysql2');
const bcrypt = require('bcryptjs');

const db = mysql.createConnection({
  host: process.env.DB_HOST || 'localhost',
  port: Number(process.env.DB_PORT || 3306),
  user: process.env.DB_USER || 'root',
  password: process.env.DB_PASSWORD || '',
  database: process.env.DB_NAME || 'digital_notice_board',
  ssl: process.env.DB_SSL === 'true' ? {
    rejectUnauthorized: true,
    ...(process.env.DB_SSL_CA ? { ca: process.env.DB_SSL_CA.replace(/\\n/g, '\n') } : {})
  } : undefined
});

async function resetPasswords() {
  console.log('Resetting passwords...\n');

  const credentials = [
    { username: 'admin', password: 'admin123' },
    { username: 'chairman', password: 'chairman123' },
  ];

  for (const cred of credentials) {
    const hash = await bcrypt.hash(cred.password, 10);
    await new Promise((resolve, reject) => {
      db.query(
        'UPDATE users SET password = ? WHERE username = ?',
        [hash, cred.username],
        (err, result) => {
          if (err) { console.error(`Failed to update ${cred.username}:`, err.message); reject(err); return; }
          if (result.affectedRows === 0) {
            console.log(`  User '${cred.username}' not found — inserting...`);
            db.query(
              "INSERT INTO users (username, password, role) VALUES (?, ?, ?)",
              [cred.username, hash, cred.username],
              (err2) => {
                if (err2) { console.error(`Failed to insert ${cred.username}:`, err2.message); reject(err2); return; }
                console.log(`  Created user '${cred.username}' with password '${cred.password}'`);
                resolve();
              }
            );
            return;
          }
          console.log(`  Updated '${cred.username}' -> password: '${cred.password}'`);
          resolve();
        }
      );
    });
  }

  console.log('\nDone! You can now login with:');
  console.log('  admin     / admin123');
  console.log('  chairman  / chairman123');
  db.end();
}

db.connect((err) => {
  if (err) { console.error('Database connection failed:', err.message); process.exit(1); }
  console.log('Connected to database.');
  resetPasswords().catch(e => { console.error(e); db.end(); });
});
