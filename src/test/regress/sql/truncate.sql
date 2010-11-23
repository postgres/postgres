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

-- Test foreign-key checks
CREATE TABLE trunc_b (a int REFERENCES truncate_a);
CREATE TABLE trunc_c (a serial PRIMARY KEY);
CREATE TABLE trunc_d (a int REFERENCES trunc_c);
CREATE TABLE trunc_e (a int REFERENCES truncate_a, b int REFERENCES trunc_c);

TRUNCATE TABLE truncate_a;		-- fail
TRUNCATE TABLE truncate_a,trunc_b;		-- fail
TRUNCATE TABLE truncate_a,trunc_b,trunc_e;	-- ok
TRUNCATE TABLE truncate_a,trunc_e;		-- fail
TRUNCATE TABLE trunc_c;		-- fail
TRUNCATE TABLE trunc_c,trunc_d;		-- fail
TRUNCATE TABLE trunc_c,trunc_d,trunc_e;	-- ok
TRUNCATE TABLE trunc_c,trunc_d,trunc_e,truncate_a;	-- fail
TRUNCATE TABLE trunc_c,trunc_d,trunc_e,truncate_a,trunc_b;	-- ok

TRUNCATE TABLE truncate_a RESTRICT; -- fail
TRUNCATE TABLE truncate_a CASCADE;  -- ok

-- circular references
ALTER TABLE truncate_a ADD FOREIGN KEY (col1) REFERENCES trunc_c;

-- Add some data to verify that truncating actually works ...
INSERT INTO trunc_c VALUES (1);
INSERT INTO truncate_a VALUES (1);
INSERT INTO trunc_b VALUES (1);
INSERT INTO trunc_d VALUES (1);
INSERT INTO trunc_e VALUES (1,1);
TRUNCATE TABLE trunc_c;
TRUNCATE TABLE trunc_c,truncate_a;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d,trunc_e;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d,trunc_e,trunc_b;

-- Verify that truncating did actually work
SELECT * FROM truncate_a
   UNION ALL
 SELECT * FROM trunc_c
   UNION ALL
 SELECT * FROM trunc_b
   UNION ALL
 SELECT * FROM trunc_d;
SELECT * FROM trunc_e;

-- Add data again to test TRUNCATE ... CASCADE
INSERT INTO trunc_c VALUES (1);
INSERT INTO truncate_a VALUES (1);
INSERT INTO trunc_b VALUES (1);
INSERT INTO trunc_d VALUES (1);
INSERT INTO trunc_e VALUES (1,1);

TRUNCATE TABLE trunc_c CASCADE;  -- ok

SELECT * FROM truncate_a
   UNION ALL
 SELECT * FROM trunc_c
   UNION ALL
 SELECT * FROM trunc_b
   UNION ALL
 SELECT * FROM trunc_d;
SELECT * FROM trunc_e;

DROP TABLE truncate_a,trunc_c,trunc_b,trunc_d,trunc_e CASCADE;

-- Test TRUNCATE with inheritance

CREATE TABLE trunc_f (col1 integer primary key);
INSERT INTO trunc_f VALUES (1);
INSERT INTO trunc_f VALUES (2);

CREATE TABLE trunc_fa (col2a text) INHERITS (trunc_f);
INSERT INTO trunc_fa VALUES (3, 'three');

CREATE TABLE trunc_fb (col2b int) INHERITS (trunc_f);
INSERT INTO trunc_fb VALUES (4, 444);

CREATE TABLE trunc_faa (col3 text) INHERITS (trunc_fa);
INSERT INTO trunc_faa VALUES (5, 'five', 'FIVE');

BEGIN;
SELECT * FROM trunc_f;
TRUNCATE trunc_f;
SELECT * FROM trunc_f;
ROLLBACK;

BEGIN;
SELECT * FROM trunc_f;
TRUNCATE ONLY trunc_f;
SELECT * FROM trunc_f;
ROLLBACK;

BEGIN;
SELECT * FROM trunc_f;
SELECT * FROM trunc_fa;
SELECT * FROM trunc_faa;
TRUNCATE ONLY trunc_fb, ONLY trunc_fa;
SELECT * FROM trunc_f;
SELECT * FROM trunc_fa;
SELECT * FROM trunc_faa;
ROLLBACK;

BEGIN;
SELECT * FROM trunc_f;
SELECT * FROM trunc_fa;
SELECT * FROM trunc_faa;
TRUNCATE ONLY trunc_fb, trunc_fa;
SELECT * FROM trunc_f;
SELECT * FROM trunc_fa;
SELECT * FROM trunc_faa;
ROLLBACK;

DROP TABLE trunc_f CASCADE;

-- Test ON TRUNCATE triggers

CREATE TABLE trunc_trigger_test (f1 int, f2 text, f3 text);
CREATE TABLE trunc_trigger_log (tgop text, tglevel text, tgwhen text,
        tgargv text, tgtable name, rowcount bigint);

CREATE FUNCTION trunctrigger() RETURNS trigger as $$
declare c bigint;
begin
    execute 'select count(*) from ' || quote_ident(tg_table_name) into c;
    insert into trunc_trigger_log values
      (TG_OP, TG_LEVEL, TG_WHEN, TG_ARGV[0], tg_table_name, c);
    return null;
end;
$$ LANGUAGE plpgsql;

-- basic before trigger
INSERT INTO trunc_trigger_test VALUES(1, 'foo', 'bar'), (2, 'baz', 'quux');

CREATE TRIGGER t
BEFORE TRUNCATE ON trunc_trigger_test
FOR EACH STATEMENT
EXECUTE PROCEDURE trunctrigger('before trigger truncate');

SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;
TRUNCATE trunc_trigger_test;
SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;

DROP TRIGGER t ON trunc_trigger_test;

truncate trunc_trigger_log;

-- same test with an after trigger
INSERT INTO trunc_trigger_test VALUES(1, 'foo', 'bar'), (2, 'baz', 'quux');

CREATE TRIGGER tt
AFTER TRUNCATE ON trunc_trigger_test
FOR EACH STATEMENT
EXECUTE PROCEDURE trunctrigger('after trigger truncate');

SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;
TRUNCATE trunc_trigger_test;
SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;

DROP TABLE trunc_trigger_test;
DROP TABLE trunc_trigger_log;

DROP FUNCTION trunctrigger();

-- test TRUNCATE ... RESTART IDENTITY
CREATE SEQUENCE truncate_a_id1 START WITH 33;
CREATE TABLE truncate_a (id serial,
                         id1 integer default nextval('truncate_a_id1'));
ALTER SEQUENCE truncate_a_id1 OWNED BY truncate_a.id1;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

TRUNCATE truncate_a;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

TRUNCATE truncate_a RESTART IDENTITY;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

-- check rollback of a RESTART IDENTITY operation
BEGIN;
TRUNCATE truncate_a RESTART IDENTITY;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;
ROLLBACK;
INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

DROP TABLE truncate_a;

SELECT nextval('truncate_a_id1'); -- fail, seq should have been dropped
