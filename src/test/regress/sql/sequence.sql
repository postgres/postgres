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

