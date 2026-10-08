const express = require('express');
const mysql   = require('mysql2');
const mqtt    = require('mqtt');
const cors    = require('cors');
const bcrypt  = require('bcryptjs');

const app = express();
app.use(cors());
app.use(express.json());

// Serve home.html as the default page
app.get('/', (req, res) => res.sendFile(__dirname + '/home.html'));

app.use(express.static('.'));

// ---- MySQL Connection Pool ----
const db = mysql.createPool({
  connectionLimit: 10,
  host:     'localhost',
  user:     'root',
  password: '1234567890-=1234567890-=',
  database: 'digital_notice_board',
  waitForConnections:    true,
  enableKeepAlive:       true,
  keepAliveInitialDelay: 0,
  connectTimeout:        10000
});
setInterval(() => {
  db.query('SELECT 1', (err) => {
    if (err) console.log('⚠️ DB keep-alive ping failed:', err.message);
    else     console.log('💓 DB keep-alive OK');
  });
}, 5 * 60 * 1000);
console.log('✅ MySQL connection pool created (limit: 10 connections)!');

// ---- MQTT Connection ----
// Using public broker.hivemq.com (Eclipse HiveMQ public broker)
const mqttClient = mqtt.connect('mqtt://broker.hivemq.com:1883', {
  reconnectPeriod: 5000,
  connectTimeout: 10000,
  clientId: 'notice_board_server'
});

mqttClient.on('connect', () => {
  console.log('✅ MQTT Connected to broker.hivemq.com:1883 (Eclipse HiveMQ public broker)!');
});

mqttClient.on('reconnect', () => {
  console.log('🔄 MQTT Reconnecting...');
});

mqttClient.on('disconnect', () => {
  console.log('❌ MQTT Disconnected');
});

mqttClient.on('error', (err) => {
  console.log('❌ MQTT Error:', err);
});

// ========================================
// AUTHENTICATION ROUTE
// ========================================

app.post('/login', (req, res) => {
  const { username, password } = req.body;

  if (!username || !password) {
    return res.json({ success: false, message: 'Username and password required' });
  }

  const sql = 'SELECT user_id, username, password, role FROM users WHERE username = ?';
  
  db.query(sql, [username], async (err, results) => {
    if (err) {
      console.log('❌ Login error:', err);
      return res.json({ success: false, message: 'Server error' });
    }

    if (results.length === 0) {
      return res.json({ success: false, message: 'Invalid username or password' });
    }

    const user = results[0];

    try {
      const isValid = await bcrypt.compare(password, user.password);
      
      if (isValid) {
        res.json({ 
          success: true, 
          username: user.username, 
          role: user.role 
        });
      } else {
        res.json({ success: false, message: 'Invalid username or password' });
      }
    } catch (err) {
      console.log('❌ bcrypt error:', err);
      res.json({ success: false, message: 'Server error' });
    }
  });
});

// ========================================
// AUTO DELETE EXPIRED ANNOUNCEMENTS
// ========================================

function checkExpiredAnnouncements() {

  console.log('⏱️ Checking for expired announcements...');

  const findSql = `
    SELECT announcement_id, title, end_datetime
    FROM announcements
    WHERE status = 'approved'
    AND end_datetime <= NOW()
  `;

  db.query(findSql, (err, results) => {

    if (err) {
      console.log('❌ Error checking expired:', err);
      return;
    }

    console.log(`📊 Found ${results.length} expired announcement(s)`);

    if (results.length > 0) {

      results.forEach(r => {
        console.log(`🗑️ Expired: "${r.title}"`);
      });

      const deleteSql = `
        DELETE FROM announcements
        WHERE status = 'approved'
        AND end_datetime <= NOW()
      `;

      db.query(deleteSql, (err) => {

        if (err) {
          console.log('❌ Error deleting expired:', err);
          return;
        }

        console.log('✅ Expired announcements deleted!');

        const payload = JSON.stringify({
          title: '',
          message: '',
          category: 'cleared',
          timestamp: new Date().toISOString()
        });

        mqttClient.publish(
          'department/notices',
          payload,
          { retain: true, qos: 1 },
          (err) => {

            if (err) {
              console.log('❌ MQTT publish error:', err);
            } else {
              console.log('📡 ✅ Clear signal sent to ESP32!');
            }

          }
        );

      });

    }

  });

}

// Check every 30 seconds (increased frequency)
setInterval(checkExpiredAnnouncements, 30000);
console.log('⏰ Auto-delete checker started! Running every 30 seconds');

// ========================================
// TEACHERS ROUTES
// ========================================

app.get('/teachers', (req, res) => {
  db.query('SELECT teacher_id, teacher_name FROM teachers ORDER BY teacher_name', (err, results) => {
    if (err) {
      console.log('❌ Error fetching teachers:', err);
      return res.json([]);
    }
    res.json(results);
  });
});

app.post('/teachers', (req, res) => {
  const { teacher_name } = req.body;
  if (!teacher_name || !teacher_name.trim()) {
    return res.json({ success: false, message: '❌ Teacher name required' });
  }

  db.query(
    'INSERT INTO teachers (teacher_name) VALUES (?)',
    [teacher_name.trim()],
    (err) => {
      if (err) {
        if (err.code === 'ER_DUP_ENTRY') {
          return res.json({ success: false, message: '❌ Teacher already exists' });
        }
        return res.json({ success: false, message: '❌ Error adding teacher' });
      }
      res.json({ success: true, message: '✅ Teacher added!' });
    }
  );
});

app.delete('/teachers/:id', (req, res) => {
  db.query(
    'DELETE FROM teachers WHERE teacher_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// ========================================
// ANNOUNCEMENTS ROUTES
// ========================================

app.get('/announcements', (req, res) => {
  db.query(
    'SELECT * FROM announcements ORDER BY created_at DESC',
    (err, results) => {
      if (err) return res.json([]);
      res.json(results);
    }
  );
});

// Get announcements by category (for ESP32 display) - only approved non-expired
app.get('/announcements/category/:category', (req, res) => {
  const category = req.params.category;
  const sql = `
    SELECT announcement_id, title, message, category, created_at, start_datetime, end_datetime
    FROM announcements
    WHERE status = 'approved'
    AND category = ?
    AND start_datetime <= NOW()
    AND end_datetime > NOW()
    ORDER BY created_at DESC
  `;
  
  db.query(sql, [category], (err, results) => {
    if (err) {
      console.log('❌ Error fetching announcements by category:', err);
      return res.json([]);
    }
    console.log(`✅ Fetched ${results.length} approved announcements for category: ${category}`);
    res.json(results);
  });
});

// Get all approved announcements (for ESP32)
app.get('/announcements/approved/all', (req, res) => {
  const sql = `
    SELECT announcement_id, title, message, category, created_at, start_datetime, end_datetime
    FROM announcements
    WHERE status = 'approved'
    AND start_datetime <= NOW()
    AND end_datetime > NOW()
    ORDER BY category, created_at DESC
  `;
  
  db.query(sql, (err, results) => {
    if (err) {
      console.log('❌ Error fetching approved announcements:', err);
      return res.json([]);
    }
    console.log(`✅ Fetched ${results.length} total approved announcements`);
    res.json(results);
  });
});

// AFTER:
app.post('/announcements', (req, res) => {
  const { title, message, category, end_datetime } = req.body;

  if (!title || !message || !category || !end_datetime) {
    return res.status(400).json({ success: false, message: '❌ All fields are required.' });
  }

  const end = new Date(end_datetime);
  const now = new Date();

  if (end <= now) {
    return res.status(400).json({ success: false, message: '❌ End date must be in the future.' });
  }

  // start_datetime auto-set to NOW() — no user input needed
  const sql = `INSERT INTO announcements 
    (title, message, category, start_datetime, end_datetime, status) 
    VALUES (?, ?, ?, NOW(), ?, 'pending')`;
  db.query(sql, [title, message, category, end_datetime], (err) => {
    if (err) {
      console.error('Error adding announcement:', err);
      return res.status(500).json({ success: false, message: '❌ Error adding announcement.' });
    }
    res.json({ success: true, message: '✅ Announcement submitted for approval!' });
  });
});

app.post('/announcements/approve/:id', (req, res) => {
  const id = req.params.id;
  db.query('SELECT * FROM announcements WHERE announcement_id = ?', [id], (err, results) => {
    if (err || results.length === 0)
      return res.json({ success: false, message: '❌ Not found' });

    const notice = results[0];
    db.query(
      'UPDATE announcements SET status = "approved" WHERE announcement_id = ?',
      [id],
      (err) => {
        if (err) return res.json({ success: false, message: '❌ Error approving' });

        const payload = JSON.stringify({
          action: 'new',   
          announcement_id: notice.announcement_id,
          title:    notice.title,
          message:  notice.message,
          category: notice.category,
          timestamp: new Date().toISOString()
        });
        // Use retain: true to persist message on broker
        mqttClient.publish('department/notices', payload, { retain: true, qos: 1 }, (err) => {
          if (err) {
            console.log('❌ MQTT publish error:', err);
            return res.json({ success: false, message: '❌ Failed to send to display' });
          }
          console.log('📡 ✅ Announcement published & retained on MQTT');
          res.json({ success: true, message: '✅ Approved and sent to display!' });
        });
      }
    );
  });
});

app.delete('/announcements/:id', (req, res) => {
  db.query(
    'SELECT * FROM announcements WHERE announcement_id = ?',
    [req.params.id],
    (err, results) => {
      if (err || results.length === 0) 
        return res.json({ success: false, message: '❌ Not found' });

      const deletedAnnouncement = results[0];

      db.query(
        'DELETE FROM announcements WHERE announcement_id = ?',
        [req.params.id],
        (err) => {
          if (err) return res.json({ success: false, message: '❌ Error deleting' });
          
          console.log(`🗑️ Announcement deleted: "${deletedAnnouncement.title}" (${deletedAnnouncement.category})`);
          
          // Publish deletion notification via MQTT
          const payload = JSON.stringify({
            action: 'deleted',
            announcement_id: req.params.id,
            category: deletedAnnouncement.category,
            title: deletedAnnouncement.title,
            timestamp: new Date().toISOString()
          });
          
          if (mqttClient.connected) {
            mqttClient.publish('department/notices', payload, { retain: false, qos: 1 }, (err) => {
              if (err) {
                console.log('❌ MQTT publish error:', err);
              } else {
                console.log('📡 ✅ Delete notification sent to ESP32');
              }
            });
          } else {
            console.log('⚠️ MQTT NOT connected - cannot send delete signal');
          }
          
          res.json({ success: true, message: '✅ Announcement deleted!' });
        }
      );
    }
  );
});

// ========================================
// DIAGNOSTICS ENDPOINT
// ========================================

app.get('/diagnostics', (req, res) => {
  const status = {
    mqtt_connected: mqttClient.connected,
    mqtt_server: 'broker.hivemq.com',
    mqtt_port: 1883,
    mqtt_protocol: 'mqtt',
    mqtt_broker: 'Eclipse HiveMQ (public)',
    timestamp: new Date().toISOString(),
    database: 'checking...'
  };

  // Check database
  db.query('SELECT COUNT(*) as count FROM announcements', (err, results) => {
    if (err) {
      status.database = 'error: ' + err.message;
    } else {
      status.database = 'ok - ' + results[0].count + ' announcements';
    }

    res.json(status);
  });
});

// ========================================
// TEST ENDPOINT - FORCE CLEAR DISPLAY
// ========================================

app.post('/test/clear-display', (req, res) => {
  console.log('🧪 TEST: Sending CLEAR signal to display...');
  
  if (!mqttClient.connected) {
    return res.json({ success: false, message: '❌ MQTT not connected!' });
  }

  const payload = JSON.stringify({
    title:    'CLEARED',
    message:  'CLEARED',
    category: 'cleared',
    timestamp: new Date().toISOString()
  });
  
  mqttClient.publish('department/notices', payload, { retain: true, qos: 1 }, (err) => {
    if (err) {
      console.log('❌ MQTT publish error:', err);
      return res.json({ success: false, message: '❌ MQTT publish failed!' });
    }
    console.log('✅ ✅ RETAINED CLEAR signal published successfully!');
    console.log('   Message will stay on broker - ESP32 will receive even if reconnecting');
    res.json({ success: true, message: '✅ Clear signal sent (retained)! Check ESP32 display.' });
  });
});

// ========================================
// SESSIONS ROUTES
// ========================================

app.get('/sessions', (req, res) => {
  db.query('SELECT * FROM sessions ORDER BY year DESC', (err, results) => {
    if (err) return res.json([]);
    res.json(results);
  });
});

app.post('/sessions', (req, res) => {
  const { session_name, year, term, semester } = req.body;

  if (!session_name || !year || !term || !semester) {
    return res.json({ success: false, message: 'All fields are required' });
  }

  // Check for duplicate session (same year + term + semester)
  const checkSql = 'SELECT session_id FROM sessions WHERE year = ? AND term = ? AND semester = ?';
  db.query(checkSql, [year, term, semester], (err, results) => {
    if (err) return res.json({ success: false, message: 'Error checking duplicate session' });
    if (results.length > 0) {
      return res.json({ success: false, message: `A session for ${term} ${year} Semester ${semester} already exists` });
    }

    db.query(
      'INSERT INTO sessions (session_name, year, term, semester) VALUES (?, ?, ?, ?)',
      [session_name, year, term, semester],
      (err) => {
        if (err) return res.json({ success: false, message: 'Error adding session' });
        res.json({ success: true, message: 'Session added successfully' });
      }
    );
  });
});

app.delete('/sessions/:id', (req, res) => {
  db.query(
    'DELETE FROM sessions WHERE session_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// ========================================
// SECTIONS ROUTES
// ========================================

app.get('/sections', (req, res) => {
  const sql = `
    SELECT sc.*, COALESCE(se.session_name, CONCAT('Session ID ', sc.session_id)) AS session_name
    FROM sections sc
    LEFT JOIN sessions se ON sc.session_id = se.session_id
    ORDER BY se.session_name, sc.section_name
  `;
  db.query(sql, (err, results) => {
    if (err) return res.json([]);
    res.json(results);
  });
});

app.post('/sections', (req, res) => {
  let { session_id, section_name } = req.body;
  section_name = section_name ? section_name.trim() : '';

  if (!session_id || !section_name) {
    return res.json({ success: false, message: '❌ Session and section name are required' });
  }

  const checkSql = 'SELECT section_id FROM sections WHERE session_id = ? AND section_name = ?';
  db.query(checkSql, [session_id, section_name], (err, results) => {
    if (err) return res.json({ success: false, message: '❌ Error checking existing section' });
    if (results.length > 0) {
      return res.json({ success: false, message: '❌ Section already exists for this session' });
    }

    const insertSql = 'INSERT INTO sections (session_id, section_name) VALUES (?, ?)';
    db.query(insertSql, [session_id, section_name], (err) => {
      if (err) return res.json({ success: false, message: '❌ Error adding' });
      res.json({ success: true, message: '✅ Section added!' });
    });
  });
});

app.delete('/sections/:id', (req, res) => {
  db.query(
    'DELETE FROM sections WHERE section_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// Get sections for a specific session
app.get('/sections/:sessionId', (req, res) => {
  const sessionId = req.params.sessionId;
  const sql = `
    SELECT DISTINCT section_id, section_name
    FROM sections
    WHERE session_id = ?
    ORDER BY section_id ASC
  `;
  
  db.query(sql, [sessionId], (err, results) => {
    if (err) {
      console.log('❌ Error fetching sections for session:', err);
      return res.json([]);
    }
    console.log(`✅ Fetched ${results.length} sections for session ${sessionId}`);
    res.json(results);
  });
});

// ========================================
// YEARS ENDPOINTS (For ESP32 Timetable Navigation)
// ========================================

// Get all years/batches from sessions table
app.get('/years', (req, res) => {
  const sql = `
    SELECT DISTINCT year as year_id, CONCAT(year, '') as year_name
    FROM sessions
    WHERE year IS NOT NULL
    ORDER BY year DESC
  `;
  
  db.query(sql, (err, results) => {
    if (err) {
      console.log('❌ Error fetching years:', err);
      return res.json([]);
    }
    console.log(`✅ Fetched ${results.length} years`);
    res.json(results);
  });
});

// Get sessions for a specific year
app.get('/sessions/:yearId', (req, res) => {
  const yearId = req.params.yearId;
  const sql = `
    SELECT session_id, session_name
    FROM sessions
    WHERE year = ?
    ORDER BY session_id DESC
  `;
  
  db.query(sql, [yearId], (err, results) => {
    if (err) {
      console.log('❌ Error fetching sessions for year:', err);
      return res.json([]);
    }
    console.log(`✅ Fetched ${results.length} sessions for year ${yearId}`);
    res.json(results);
  });
});

// ========================================
// SUBJECTS ROUTES
// ========================================

app.get('/subjects', (req, res) => {
  db.query('SELECT * FROM subjects ORDER BY subject_name', (err, results) => {
    if (err) return res.json([]);
    res.json(results);
  });
});

app.post('/subjects', (req, res) => {
  const { subject_name, subject_code } = req.body;
  db.query(
    'INSERT INTO subjects (subject_name, subject_code) VALUES (?, ?)',
    [subject_name, subject_code],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error adding' });
      res.json({ success: true, message: '✅ Subject added!' });
    }
  );
});

app.delete('/subjects/:id', (req, res) => {
  db.query(
    'DELETE FROM subjects WHERE subject_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// ========================================
// ROOMS ROUTES
// ========================================

app.get('/rooms', (req, res) => {
  db.query('SELECT * FROM rooms ORDER BY room_number', (err, results) => {
    if (err) return res.json([]);
    res.json(results);
  });
});

app.post('/rooms', (req, res) => {
  const { room_number } = req.body;
  db.query(
    'INSERT INTO rooms (room_number) VALUES (?)',
    [room_number],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error adding' });
      res.json({ success: true, message: '✅ Room added!' });
    }
  );
});

app.delete('/rooms/:id', (req, res) => {
  db.query(
    'DELETE FROM rooms WHERE room_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// ========================================
// DAYS ROUTE
// ========================================

app.get('/days', (req, res) => {
  db.query('SELECT * FROM days ORDER BY day_id', (err, results) => {
    if (err) return res.json([]);
    res.json(results);
  });
});

// ========================================
// TIMETABLE ROUTES
// ========================================

// Get all timetables
app.get('/timetable', (req, res) => {
  const sql = `
    SELECT 
      t.timetable_id,
      se.session_name,
      se.session_id,
      sc.section_name,
      sc.section_id,
      d.day_name,
      d.day_id,
      t.start_time,
      t.end_time,
      t.duration_hours,
      su.subject_name,
      su.subject_code,
      t.teacher_id,
      te.teacher_name,
      r.room_number
    FROM timetable t
    JOIN sessions se ON t.session_id = se.session_id
    JOIN sections sc ON t.section_id = sc.section_id
    JOIN days     d  ON t.day_id     = d.day_id
    JOIN subjects su ON t.subject_id = su.subject_id
    JOIN rooms    r  ON t.room_id    = r.room_id
    LEFT JOIN teachers te ON t.teacher_id = te.teacher_id
    ORDER BY se.session_id, sc.section_id, d.day_id, t.start_time
  `;
  db.query(sql, (err, results) => {
    if (err) {
      console.log('❌ Error fetching timetable:', err);
      return res.json([]);
    }
    res.json(results);
  });
});

// Get timetable for specific year, session, section (for ESP32)
app.get('/timetable/:year_id/:session_id/:section_id', (req, res) => {
  const { year_id, session_id, section_id } = req.params;

  const sql = `
    SELECT 
      t.timetable_id,
      t.start_time,
      t.end_time,
      TIMEDIFF(t.end_time, t.start_time) as duration,
      su.subject_name,
      su.subject_code,
      t.teacher_id,
      IFNULL(te.teacher_name, 'TBA') as teacher_name,
      r.room_number,
      d.day_name,
      d.day_id,
      se.semester,
      se.session_name,
      se.term,
      sc.section_name
    FROM timetable t
    JOIN sessions se ON t.session_id = se.session_id
    JOIN sections sc ON t.section_id = sc.section_id
    JOIN days     d  ON t.day_id     = d.day_id
    JOIN subjects su ON t.subject_id = su.subject_id
    JOIN rooms    r  ON t.room_id    = r.room_id
    LEFT JOIN teachers te ON t.teacher_id = te.teacher_id
    WHERE se.year = ? AND se.session_id = ? AND sc.section_id = ?
    ORDER BY d.day_id, TIME(t.start_time) ASC
  `;

  db.query(sql, [year_id, session_id, section_id], (err, results) => {
    if (err) {
      console.log('❌ Error fetching timetable:', err);
      return res.json({ success: false, data: [], semester: "Unknown", term: "Unknown", session_name: "Unknown" });
    }

    let semester = "Unknown";
    let term = "Unknown";
    let session_name = "Unknown";
    if (results.length > 0) {
      semester     = results[0].semester     || "Unknown";
      term         = results[0].term         || "Unknown";
      session_name = results[0].session_name || "Unknown";
    }

    console.log(`✅ Fetched ${results.length} timetable entries | ${semester} - ${term} (${session_name})`);
    res.json({
      success: true,
      semester: semester,
      term: term,
      session_name: session_name,
      data: results
    });
  });
});

// Get timetable for specific year, session, section, and day (for ESP32)
app.get('/timetable/:year_id/:session_id/:section_id/:day_id', (req, res) => {
  const { year_id, session_id, section_id, day_id } = req.params;
  
  const sql = `
    SELECT 
      t.timetable_id,
      t.start_time,
      t.end_time,
      TIMEDIFF(t.end_time, t.start_time) as duration,
      su.subject_name,
      su.subject_code,
      t.teacher_id,
      IFNULL(te.teacher_name, 'TBA') as teacher_name,
      r.room_number,
      d.day_name,
      d.day_id,
      se.semester,
      se.session_name,
      se.term,
      sc.section_name
    FROM timetable t
    JOIN sessions se ON t.session_id = se.session_id
    JOIN sections sc ON t.section_id = sc.section_id
    JOIN days     d  ON t.day_id     = d.day_id
    JOIN subjects su ON t.subject_id = su.subject_id
    JOIN rooms    r  ON t.room_id    = r.room_id
    LEFT JOIN teachers te ON t.teacher_id = te.teacher_id
    WHERE se.year = ? AND se.session_id = ? AND sc.section_id = ? AND d.day_id = ?
    ORDER BY TIME(t.start_time) ASC
  `;
  
  db.query(sql, [year_id, session_id, section_id, day_id], (err, results) => {
    if (err) {
      console.log('❌ Error fetching timetable:', err);
      return res.json({ success: false, data: [], semester: "Unknown", term: "Unknown", session_name: "Unknown" });
    }
    
    let semester = "Unknown";
    let term = "Unknown";
    let session_name = "Unknown";
    if (results.length > 0) {
      semester = results[0].semester || "Unknown";
      term = results[0].term || "Unknown";
      session_name = results[0].session_name || "Unknown";
    }
    
    console.log(`✅ Fetched ${results.length} timetable entries for day ${day_id} | ${semester} - ${term} (${session_name})`);
    res.json({ 
      success: true, 
      semester: semester,
      term: term,
      session_name: session_name,
      data: results 
    });
  });
});

// Get timetable for specific session and section (for ESP32 - legacy endpoint)
app.get('/timetable/:session_id/:section_id', (req, res) => {
  const { session_id, section_id } = req.params;
  
  const sql = `
    SELECT 
      t.start_time,
      t.end_time,
      su.subject_name,
      su.subject_code,
      t.teacher_id,
      te.teacher_name,
      r.room_number,
      d.day_name,
      d.day_id
    FROM timetable t
    JOIN sessions se ON t.session_id = se.session_id
    JOIN sections sc ON t.section_id = sc.section_id
    JOIN days     d  ON t.day_id     = d.day_id
    JOIN subjects su ON t.subject_id = su.subject_id
    JOIN rooms    r  ON t.room_id    = r.room_id
    LEFT JOIN teachers te ON t.teacher_id = te.teacher_id
    WHERE se.session_id = ? AND sc.section_id = ?
    ORDER BY d.day_id, t.start_time
  `;
  
  db.query(sql, [session_id, section_id], (err, results) => {
    if (err) {
      console.log('❌ Error fetching timetable:', err);
      return res.json({ success: false, data: [] });
    }
    
    console.log(`✅ Fetched ${results.length} timetable entries for session ${session_id}, section ${section_id}`);
    res.json({ success: true, data: results });
  });
});

// Publish all timetables to MQTT (for ESP32)
app.post('/timetable/sync/mqtt', (req, res) => {
  if (!mqttClient.connected) {
    return res.json({ success: false, message: '❌ MQTT not connected!' });
  }

  const sql = `
    SELECT 
      se.session_id,
      sc.section_id,
      t.start_time,
      t.end_time,
      su.subject_name,
      te.teacher_name,
      r.room_number,
      d.day_name
    FROM timetable t
    JOIN sessions se ON t.session_id = se.session_id
    JOIN sections sc ON t.section_id = sc.section_id
    JOIN days     d  ON t.day_id     = d.day_id
    JOIN subjects su ON t.subject_id = su.subject_id
    JOIN rooms    r  ON t.room_id    = r.room_id
    LEFT JOIN teachers te ON t.teacher_id = te.teacher_id
    ORDER BY se.session_id, sc.section_id, d.day_id, t.start_time
  `;

  db.query(sql, (err, results) => {
    if (err) {
      console.log('❌ Error fetching timetable:', err);
      return res.json({ success: false, message: '❌ Database error' });
    }

    if (results.length === 0) {
      return res.json({ success: false, message: '❌ No timetable data found' });
    }

    let publishCount = 0;
    const totalToPublish = results.length;

    results.forEach((entry, index) => {
      const payload = JSON.stringify({
        category: 'timetable',
        session: entry.session_id - 1,  // 0=Morning, 1=Evening
        section: entry.section_id - 1,  // 0=A, 1=B, 2=C
        entry: index,
        time: `${entry.start_time} - ${entry.end_time}`,
        subject: entry.subject_name,
        teacher: entry.teacher_name,
        room: entry.room_number,
        day: entry.day_name
      });

      mqttClient.publish('department/notices', payload, { retain: true, qos: 1 }, (err) => {
        if (!err) publishCount++;
        
        if (publishCount === totalToPublish) {
          console.log(`📡 ✅ Published ${publishCount} timetable entries to MQTT!`);
          res.json({ 
            success: true, 
            message: `✅ Published ${publishCount} timetable entries!`,
            published: publishCount
          });
        }
      });
    });
  });
});

app.post('/timetable', (req, res) => {
  const { session_id, section_id, day_id, start_time,
          end_time, duration_hours, subject_id, teacher_id, room_id } = req.body;

  // Validation: Check for room conflicts
  const roomConflictSql = `
    SELECT COUNT(*) as count FROM timetable 
    WHERE day_id = ? AND room_id = ?
    AND (
      (start_time < ? AND end_time > ?)
      OR (start_time >= ? AND start_time < ?)
      OR (end_time > ? AND end_time <= ?)
    )
  `;

  db.query(roomConflictSql, 
    [day_id, room_id, end_time, start_time, start_time, end_time, start_time, end_time],
    (err, results) => {
      if (err) return res.json({ success: false, message: '❌ Error checking room conflicts' });

      if (results[0].count > 0) {
        return res.json({ 
          success: false, 
          message: '❌ Room is already allocated for this time slot!' 
        });
      }

      // Validation: Check for teacher conflicts
      const teacherConflictSql = `
        SELECT COUNT(*) as count FROM timetable 
        WHERE day_id = ? AND teacher_id = ?
        AND (
          (start_time < ? AND end_time > ?)
          OR (start_time >= ? AND start_time < ?)
          OR (end_time > ? AND end_time <= ?)
        )
      `;

      db.query(teacherConflictSql, 
        [day_id, teacher_id, end_time, start_time, start_time, end_time, start_time, end_time],
        (err, results) => {
          if (err) return res.json({ success: false, message: '❌ Error checking teacher conflicts' });

          if (results[0].count > 0) {
            return res.json({ 
              success: false, 
              message: '❌ Teacher is already allocated for this time slot!' 
            });
          }

          // If no conflicts, insert the entry
          const insertSql = `INSERT INTO timetable 
            (session_id, section_id, day_id, start_time, end_time, 
             duration_hours, subject_id, teacher_id, room_id) 
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`;
          
          db.query(insertSql,
            [session_id, section_id, day_id, start_time,
             end_time, duration_hours, subject_id, teacher_id, room_id],
            (err) => {
              if (err) return res.json({ success: false, message: '❌ Error adding' });
              res.json({ success: true, message: '✅ Timetable entry added!' });
            }
          );
        }
      );
    }
  );
});

app.put('/timetable/:id', (req, res) => {
  const { session_id, section_id, day_id, start_time,
          end_time, duration_hours, subject_id, teacher_id, room_id } = req.body;

  // Validation: Check for room conflicts (excluding current entry)
  const roomConflictSql = `
    SELECT COUNT(*) as count FROM timetable 
    WHERE day_id = ? AND room_id = ? AND timetable_id != ?
    AND (
      (start_time < ? AND end_time > ?)
      OR (start_time >= ? AND start_time < ?)
      OR (end_time > ? AND end_time <= ?)
    )
  `;

  db.query(roomConflictSql, 
    [day_id, room_id, req.params.id, end_time, start_time, start_time, end_time, start_time, end_time],
    (err, results) => {
      if (err) return res.json({ success: false, message: '❌ Error checking room conflicts' });

      if (results[0].count > 0) {
        return res.json({ 
          success: false, 
          message: '❌ Room is already allocated for this time slot!' 
        });
      }

      // Validation: Check for teacher conflicts (excluding current entry)
      const teacherConflictSql = `
        SELECT COUNT(*) as count FROM timetable 
        WHERE day_id = ? AND teacher_id = ? AND timetable_id != ?
        AND (
          (start_time < ? AND end_time > ?)
          OR (start_time >= ? AND start_time < ?)
          OR (end_time > ? AND end_time <= ?)
        )
      `;

      db.query(teacherConflictSql, 
        [day_id, teacher_id, req.params.id, end_time, start_time, start_time, end_time, start_time, end_time],
        (err, results) => {
          if (err) return res.json({ success: false, message: '❌ Error checking teacher conflicts' });

          if (results[0].count > 0) {
            return res.json({ 
              success: false, 
              message: '❌ Teacher is already allocated for this time slot!' 
            });
          }

          // If no conflicts, update the entry
          const updateSql = `UPDATE timetable SET 
            session_id=?, section_id=?, day_id=?, start_time=?,
            end_time=?, duration_hours=?, subject_id=?, teacher_id=?, room_id=?
            WHERE timetable_id=?`;
          
          db.query(updateSql,
            [session_id, section_id, day_id, start_time,
             end_time, duration_hours, subject_id, teacher_id, room_id, req.params.id],
            (err) => {
              if (err) return res.json({ success: false, message: '❌ Error updating' });
              res.json({ success: true, message: '✅ Updated!' });
            }
          );
        }
      );
    }
  );
});

app.delete('/timetable/:id', (req, res) => {
  db.query(
    'DELETE FROM timetable WHERE timetable_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// ========================================
// TEACHER OFFICES ROUTES
// ========================================

app.get('/offices', (req, res) => {
  db.query(
    'SELECT o.office_id, o.teacher_id, t.teacher_name, o.floor, o.room_number, o.cabin_number FROM teacher_offices o LEFT JOIN teachers t ON o.teacher_id = t.teacher_id ORDER BY o.floor, o.room_number, o.cabin_number',
    (err, results) => {
      if (err) return res.json([]);
      res.json(results);
    }
  );
});

app.post('/offices', (req, res) => {
  const { teacher_id, floor, room_number, cabin_number } = req.body;
  if (!teacher_id) {
    return res.json({ success: false, message: '❌ Teacher ID required' });
  }
  db.query(
    'INSERT INTO teacher_offices (teacher_id, floor, room_number, cabin_number) VALUES (?, ?, ?, ?)',
    [teacher_id, floor, room_number, cabin_number],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error adding' });
      res.json({ success: true, message: '✅ Office added!' });
    }
  );
});

app.put('/offices/:id', (req, res) => {
  const { teacher_id, floor, room_number, cabin_number } = req.body;
  db.query(
    'UPDATE teacher_offices SET teacher_id=?, floor=?, room_number=?, cabin_number=? WHERE office_id=?',
    [teacher_id, floor, room_number, cabin_number, req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error updating' });
      res.json({ success: true, message: '✅ Updated!' });
    }
  );
});

app.delete('/offices/:id', (req, res) => {
  db.query(
    'DELETE FROM teacher_offices WHERE office_id = ?',
    [req.params.id],
    (err) => {
      if (err) return res.json({ success: false, message: '❌ Error deleting' });
      res.json({ success: true, message: '✅ Deleted!' });
    }
  );
});

// GET all teachers with their office locations
app.get('/teachers/offices', (req, res) => {
  const query = `
    SELECT 
      t.teacher_id AS id, 
      t.teacher_name AS name, 
      o.room_number AS room_no, 
      o.cabin_number AS cabin_no,
      CASE
        WHEN o.floor = 'Ground Floor' THEN 0
        WHEN o.floor = 'First Floor' THEN 1
        ELSE -1 
      END AS floor
    FROM teachers t
    JOIN teacher_offices o ON t.teacher_id = o.teacher_id
    ORDER BY t.teacher_name;
  `;

  db.query(query, (err, results) => {
    if (err) {
      console.error("Error fetching teachers' offices:", err);
      return res.status(500).json({ success: false, message: "Error fetching teachers' offices" });
    }
    console.log(`✅ Fetched ${results.length} teachers with offices`);
    res.json(results);
  });
});

// ---- Start Server ----
app.listen(3000, () => {
  console.log('🚀 Server running at http://localhost:3000');
});

// ---- mDNS Advertisement ----
const bonjour = require('bonjour')();
bonjour.publish({ name: 'noticeboard', type: 'http', port: 3000 });
console.log('📡 mDNS started — server reachable at noticeboard.local:3000');