--
-- Create a new user with the next unused usesysid
--
CREATE FUNCTION viewperms_nextid () RETURNS int4 AS '
	SELECT max(usesysid) + 1 AS ret FROM pg_user;
    ' LANGUAGE 'sql';

CREATE FUNCTION viewperms_testid () RETURNS oid AS '
    	SELECT oid(textin(int4out(usesysid))) FROM pg_user 
		WHERE usename = ''viewperms_testuser'';
    ' LANGUAGE 'sql';

INSERT INTO pg_shadow VALUES (
	'viewperms_testuser',
	viewperms_nextid(),
	false, true, false, true,
	NULL, NULL
    );

--
-- Create tables and views
--
CREATE TABLE viewperms_t1 (
    	a	int4,
	b	text
    );

CREATE TABLE viewperms_t2 (
    	a	int4,
	b	text
    );

INSERT INTO viewperms_t1 VALUES (1, 'one');
INSERT INTO viewperms_t1 VALUES (2, 'two');
INSERT INTO viewperms_t1 VALUES (3, 'three');

INSERT INTO viewperms_t2 VALUES (1, 'one');
INSERT INTO viewperms_t2 VALUES (2, 'two');
INSERT INTO viewperms_t2 VALUES (3, 'three');

CREATE VIEW viewperms_v1 AS SELECT * FROM viewperms_t1;
CREATE VIEW viewperms_v2 AS SELECT * FROM viewperms_t2;
CREATE VIEW viewperms_v3 AS SELECT * FROM viewperms_t1;
CREATE VIEW viewperms_v4 AS SELECT * FROM viewperms_t2;
CREATE VIEW viewperms_v5 AS SELECT * FROM viewperms_v1;
CREATE VIEW viewperms_v6 AS SELECT * FROM viewperms_v4;
CREATE VIEW viewperms_v7 AS SELECT * FROM viewperms_v2;

--
-- Change ownership
--     t1	tuser
--     t2	pgslq
--     v1	pgslq
--     v2	pgslq
--     v3	tuser
--     v4	tuser
--     v5	postgres
--     v6	postgres
--     v7	tuser
--
UPDATE pg_class SET relowner = viewperms_testid() 
	WHERE relname = 'viewperms_t1';
UPDATE pg_class SET relowner = viewperms_testid() 
	WHERE relname = 'viewperms_v3';
UPDATE pg_class SET relowner = viewperms_testid() 
	WHERE relname = 'viewperms_v4';
UPDATE pg_class SET relowner = viewperms_testid() 
	WHERE relname = 'viewperms_v7';

--
-- Now for the tests.
--

-- View v1 owner postgres has access to t1 owned by tuser
SELECT * FROM viewperms_v1;

-- View v2 owner postgres has access to t2 owned by postgres (of cause)
SELECT * FROM viewperms_v2;

-- View v3 owner tuser has access to t1 owned by tuser
SELECT * FROM viewperms_v3;

-- View v4 owner tuser has NO access to t2 owned by postgres
-- MUST fail with permission denied
SELECT * FROM viewperms_v4;

-- v5 (postgres) can access v2 (postgres) can access t1 (tuser)
SELECT * FROM viewperms_v5;

-- v6 (postgres) can access v4 (tuser) CANNOT access t2 (postgres)
SELECT * FROM viewperms_v6;

-- v7 (tuser) CANNOT access v2 (postgres) wanna access t2 (pgslq)
SELECT * FROM viewperms_v7;

GRANT SELECT ON viewperms_v2 TO PUBLIC;
-- but now
-- v7 (tuser) can access v2 (postgres via grant) can access t2 (postgres)
SELECT * FROM viewperms_v7;

--
-- Tidy up - we remove the testuser below and we don't let
-- objects lay around with bad owner reference
--
DROP VIEW viewperms_v1;
DROP VIEW viewperms_v2;
DROP VIEW viewperms_v3;
DROP VIEW viewperms_v4;
DROP VIEW viewperms_v5;
DROP VIEW viewperms_v6;
DROP VIEW viewperms_v7;
DROP TABLE viewperms_t1;
DROP TABLE viewperms_t2;
DROP FUNCTION viewperms_nextid ();
DROP FUNCTION viewperms_testid ();

--
-- Remove the testuser
--
DELETE FROM pg_shadow WHERE usename = 'viewperms_testuser';

