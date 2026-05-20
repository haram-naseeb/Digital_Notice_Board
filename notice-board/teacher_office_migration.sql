-- Create the 'offices' table to store location details
CREATE TABLE offices (
  id INT AUTO_INCREMENT PRIMARY KEY,
  room_no INT NOT NULL,
  cabin_no INT NOT NULL,
  floor INT NOT NULL, -- 0 for Ground, 1 for First
  UNIQUE(room_no, cabin_no, floor)
);

-- Add a foreign key column to the 'teachers' table to link to an office
ALTER TABLE teachers
ADD COLUMN office_id INT,
ADD CONSTRAINT fk_teacher_office
  FOREIGN KEY (office_id)
  REFERENCES offices(id);

-- =================================================================
-- ========================= SAMPLE DATA =========================
-- =================================================================

-- Insert sample office locations
-- Ground Floor (Floor 0)
INSERT INTO offices (room_no, cabin_no, floor) VALUES
(101, 1, 0), (101, 2, 0), (101, 3, 0),
(102, 4, 0), (102, 5, 0), (102, 6, 0);

-- First Floor (Floor 1)
INSERT INTO offices (room_no, cabin_no, floor) VALUES
(201, 7, 1), (201, 8, 1), (201, 9, 1),
(202, 10, 1), (202, 11, 1), (202, 12, 1);

-- Assign offices to existing teachers (example IDs, adjust as needed)
-- Replace teacher IDs (1, 2, 3, etc.) with actual IDs from your 'teachers' table
UPDATE teachers SET office_id = 1 WHERE id = 1;  -- Teacher 1 -> Ground, Cabin 1
UPDATE teachers SET office_id = 2 WHERE id = 2;  -- Teacher 2 -> Ground, Cabin 2
UPDATE teachers SET office_id = 3 WHERE id = 3;  -- Teacher 3 -> Ground, Cabin 3
UPDATE teachers SET office_id = 4 WHERE id = 4;  -- Teacher 4 -> Ground, Cabin 4
UPDATE teachers SET office_id = 5 WHERE id = 5;  -- Teacher 5 -> Ground, Cabin 5
UPDATE teachers SET office_id = 6 WHERE id = 6;  -- Teacher 6 -> Ground, Cabin 6

UPDATE teachers SET office_id = 7 WHERE id = 7;  -- Teacher 7 -> First, Cabin 7
UPDATE teachers SET office_id = 8 WHERE id = 8;  -- Teacher 8 -> First, Cabin 8
UPDATE teachers SET office_id = 9 WHERE id = 9;  -- Teacher 9 -> First, Cabin 9
UPDATE teachers SET office_id = 10 WHERE id = 10; -- Teacher 10 -> First, Cabin 10
UPDATE teachers SET office_id = 11 WHERE id = 11; -- Teacher 11 -> First, Cabin 11
UPDATE teachers SET office_id = 12 WHERE id = 12; -- Teacher 12 -> First, Cabin 12
