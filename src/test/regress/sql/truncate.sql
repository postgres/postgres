-- Test basic TRUNCATE functionality.
CREATE TABLE truncate_a (col1 integer primary key);
INSERT INTO truncate_a VALUES (1);
INSERT INTO truncate_a VALUES (2);
SELECT * FROM truncate_a;
-- Roll truncate back
BEGIN;
TRUNCATE truncate_a;
ROLLBACK;
SELECT * FROM truncate_a;
-- Commit the truncate this time
BEGIN;
TRUNCATE truncate_a;
COMMIT;
SELECT * FROM truncate_a;

-- Test foreign constraint check
CREATE TABLE truncate_b(col1 integer references truncate_a);
INSERT INTO truncate_a VALUES (1);
SELECT * FROM truncate_a;
TRUNCATE truncate_a;
SELECT * FROM truncate_a;

DROP TABLE truncate_b;
DROP TABLE truncate_a;
