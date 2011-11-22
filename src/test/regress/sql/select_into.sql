--
-- SELECT_INTO
--

SELECT *
   INTO TABLE tmp1
   FROM onek
   WHERE onek.unique1 < 2;

DROP TABLE tmp1;

SELECT *
   INTO TABLE tmp1
   FROM onek2
   WHERE onek2.unique1 < 2;

DROP TABLE tmp1;

--
-- SELECT INTO and INSERT permission, if owner is not allowed to insert.
--
CREATE SCHEMA selinto_schema;
CREATE USER selinto_user;
ALTER DEFAULT PRIVILEGES FOR ROLE selinto_user
	  REVOKE INSERT ON TABLES FROM selinto_user;
GRANT ALL ON SCHEMA selinto_schema TO public;

SET SESSION AUTHORIZATION selinto_user;
SELECT * INTO TABLE selinto_schema.tmp1
	  FROM pg_class WHERE relname like '%a%';	-- Error
SELECT oid AS clsoid, relname, relnatts + 10 AS x
	  INTO selinto_schema.tmp2
	  FROM pg_class WHERE relname like '%b%';	-- Error
CREATE TABLE selinto_schema.tmp3 (a,b,c)
	   AS SELECT oid,relname,relacl FROM pg_class
	   WHERE relname like '%c%';	-- Error
RESET SESSION AUTHORIZATION;

ALTER DEFAULT PRIVILEGES FOR ROLE selinto_user
	  GRANT INSERT ON TABLES TO selinto_user;

SET SESSION AUTHORIZATION selinto_user;
SELECT * INTO TABLE selinto_schema.tmp1
	  FROM pg_class WHERE relname like '%a%';	-- OK
SELECT oid AS clsoid, relname, relnatts + 10 AS x
	  INTO selinto_schema.tmp2
	  FROM pg_class WHERE relname like '%b%';	-- OK
CREATE TABLE selinto_schema.tmp3 (a,b,c)
	   AS SELECT oid,relname,relacl FROM pg_class
	   WHERE relname like '%c%';	-- OK
RESET SESSION AUTHORIZATION;

DROP SCHEMA selinto_schema CASCADE;
DROP USER selinto_user;
