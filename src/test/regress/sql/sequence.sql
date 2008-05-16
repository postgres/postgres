---
--- test creation of SERIAL column
---
 
CREATE TABLE serialTest (f1 text, f2 serial);
 
INSERT INTO serialTest VALUES ('foo');
INSERT INTO serialTest VALUES ('bar');
INSERT INTO serialTest VALUES ('force', 100);
INSERT INTO serialTest VALUES ('wrong', NULL);
 
SELECT * FROM serialTest;

-- basic sequence operations using both text and oid references
CREATE SEQUENCE sequence_test;
 
SELECT nextval('sequence_test'::text);
SELECT nextval('sequence_test'::regclass);
SELECT currval('sequence_test'::text);
SELECT currval('sequence_test'::regclass);
SELECT setval('sequence_test'::text, 32);
SELECT nextval('sequence_test'::regclass);
SELECT setval('sequence_test'::text, 99, false);
SELECT nextval('sequence_test'::regclass);
SELECT setval('sequence_test'::regclass, 32);
SELECT nextval('sequence_test'::text);
SELECT setval('sequence_test'::regclass, 99, false);
SELECT nextval('sequence_test'::text);

DROP SEQUENCE sequence_test;

-- renaming sequences
CREATE SEQUENCE foo_seq;
ALTER TABLE foo_seq RENAME TO foo_seq_new;
SELECT * FROM foo_seq_new;
SELECT nextval('foo_seq_new');
SELECT nextval('foo_seq_new');
SELECT * FROM foo_seq_new;
DROP SEQUENCE foo_seq_new;

-- renaming serial sequences
ALTER TABLE serialtest_f2_seq RENAME TO serialtest_f2_foo;
INSERT INTO serialTest VALUES ('more');
SELECT * FROM serialTest;

--
-- Check dependencies of serial and ordinary sequences
--
CREATE TEMP SEQUENCE myseq2;
CREATE TEMP SEQUENCE myseq3;
CREATE TEMP TABLE t1 (
  f1 serial,
  f2 int DEFAULT nextval('myseq2'),
  f3 int DEFAULT nextval('myseq3'::text)
);
-- Both drops should fail, but with different error messages:
DROP SEQUENCE t1_f1_seq;
DROP SEQUENCE myseq2;
-- This however will work:
DROP SEQUENCE myseq3;
DROP TABLE t1;
-- Fails because no longer existent:
DROP SEQUENCE t1_f1_seq;
-- Now OK:
DROP SEQUENCE myseq2;

--
-- Alter sequence
--
CREATE SEQUENCE sequence_test2 START WITH 32;

SELECT nextval('sequence_test2');

ALTER SEQUENCE sequence_test2 RESTART WITH 24
	 INCREMENT BY 4 MAXVALUE 36 MINVALUE 5 CYCLE;
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');
SELECT nextval('sequence_test2');

ALTER SEQUENCE sequence_test2 RESTART;

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