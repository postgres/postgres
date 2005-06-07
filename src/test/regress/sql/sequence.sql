---
--- test creation of SERIAL column
---
 
CREATE TABLE serialTest (f1 text, f2 serial);
 
INSERT INTO serialTest VALUES ('foo');
INSERT INTO serialTest VALUES ('bar');
INSERT INTO serialTest VALUES ('force', 100);
INSERT INTO serialTest VALUES ('wrong', NULL);
 
SELECT * FROM serialTest;
 
CREATE SEQUENCE sequence_test;
 
BEGIN;
SELECT nextval('sequence_test');
DROP SEQUENCE sequence_test;
END;

-- renaming sequences
CREATE SEQUENCE foo_seq;
ALTER TABLE foo_seq RENAME TO foo_seq_new;
SELECT * FROM foo_seq_new;
DROP SEQUENCE foo_seq_new;

--
-- Alter sequence
--
CREATE SEQUENCE sequence_test2 START WITH 32;

SELECT nextval('sequence_test2');

ALTER SEQUENCE sequence_test2 RESTART WITH 16
	 INCREMENT BY 4 MAXVALUE 22 MINVALUE 5 CYCLE;
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');

-- Test comments
COMMENT ON SEQUENCE asdf IS 'won''t work';
COMMENT ON SEQUENCE sequence_test2 IS 'will work';
COMMENT ON SEQUENCE sequence_test2 IS NULL;

-- Test lastval()
CREATE SEQUENCE seq;
SELECT nextval('seq');
SELECT lastval();
SELECT setval('seq', 99);
SELECT lastval();

CREATE SEQUENCE seq2;
SELECT nextval('seq2');
SELECT lastval();

DROP SEQUENCE seq2;
-- should fail
SELECT lastval();

CREATE USER seq_user;

BEGIN;
SET LOCAL SESSION AUTHORIZATION seq_user;
CREATE SEQUENCE seq3;
SELECT nextval('seq3');
REVOKE ALL ON seq3 FROM seq_user;
SELECT lastval();
ROLLBACK;

DROP USER seq_user;
DROP SEQUENCE seq;