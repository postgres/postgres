-- Load pg_audit module
create extension pg_audit;

--
-- Audit log fields are:
--     AUDIT_TYPE - SESSION or OBJECT
--     STATEMENT_ID - ID of the statement in the current backend
--     SUBSTATEMENT_ID - ID of the substatement in the current backend
--     CLASS - Class of statement being logged (e.g. ROLE, READ, WRITE)
--     COMMAND - e.g. SELECT, CREATE ROLE, UPDATE
--     OBJECT_TYPE - When available, type of object acted on (e.g. TABLE, VIEW)
--     OBJECT_NAME - When available, fully-qualified table of object
--     STATEMENT - The statement being logged
--     PARAMETER - If parameter logging is requested, they will follow the
--                 statement

select current_user \gset

--
-- Set pg_audit parameters for the current (super)user.
ALTER ROLE :current_user SET pg_audit.log = 'Role';
ALTER ROLE :current_user SET pg_audit.log_level = 'notice';

CREATE FUNCTION load_pg_audit( )
 RETURNS VOID
 LANGUAGE plpgsql
SECURITY DEFINER
AS $function$
declare
begin
LOAD 'pg_audit';
end;
$function$;

-- After each connect, we need to load pg_audit, as if it was
-- being loaded from shared_preload_libraries.  Otherwise, the hooks
-- won't be set up and called correctly, leading to lots of ugly
-- errors.
\connect - :current_user;
select load_pg_audit();

--
-- Create auditor role
CREATE ROLE auditor;

--
-- Create first test user
CREATE USER user1;
ALTER ROLE user1 SET pg_audit.log = 'ddl, ROLE';
ALTER ROLE user1 SET pg_audit.log_level = 'notice';

--
-- Create, select, drop (select will not be audited)
\connect - user1
select load_pg_audit();
CREATE TABLE public.test (id INT);
SELECT * FROM test;
DROP TABLE test;

--
-- Create second test user
\connect - :current_user
select load_pg_audit();

CREATE USER user2;
ALTER ROLE user2 SET pg_audit.log = 'Read, writE';
ALTER ROLE user2 SET pg_audit.log_catalog = OFF;
ALTER ROLE user2 SET pg_audit.log_level = 'warning';
ALTER ROLE user2 SET pg_audit.role = auditor;
ALTER ROLE user2 SET pg_audit.log_statement_once = ON;

\connect - user2
select load_pg_audit();
CREATE TABLE test2 (id INT);
GRANT SELECT ON TABLE public.test2 TO auditor;

--
-- Role-based tests
CREATE TABLE test3
(
	id INT
);

SELECT count(*)
  FROM
(
	SELECT relname
	  FROM pg_class
	  LIMIT 1
) SUBQUERY;

SELECT *
  FROM test3, test2;

GRANT INSERT
   ON TABLE public.test3
   TO auditor;

--
-- Create a view to test logging
CREATE VIEW vw_test3 AS
SELECT *
  FROM test3;

GRANT SELECT
   ON vw_test3
   TO auditor;

--
-- Object logged because of:
-- select on vw_test3
-- select on test2
SELECT *
  FROM vw_test3, test2;

--
-- Object logged because of:
-- insert on test3
-- select on test2
WITH CTE AS
(
	SELECT id
	  FROM test2
)
INSERT INTO test3
SELECT id
  FROM cte;

--
-- Object logged because of:
-- insert on test3
WITH CTE AS
(
	INSERT INTO test3 VALUES (1)
				   RETURNING id
)
INSERT INTO test2
SELECT id
  FROM cte;

GRANT UPDATE ON TABLE public.test2 TO auditor;

--
-- Object logged because of:
-- insert on test3
-- update on test2
WITH CTE AS
(
	UPDATE test2
	   SET id = 1
	RETURNING id
)
INSERT INTO test3
SELECT id
  FROM cte;

--
-- Object logged because of:
-- insert on test2
WITH CTE AS
(
	INSERT INTO test2 VALUES (1)
				   RETURNING id
)
UPDATE test3
   SET id = cte.id
  FROM cte
 WHERE test3.id <> cte.id;

--
-- Change permissions of user 2 so that only object logging will be done
\connect - :current_user
select load_pg_audit();
alter role user2 set pg_audit.log = 'NONE';

\connect - user2
select load_pg_audit();

--
-- Create test4 and add permissions
CREATE TABLE test4
(
	id int,
	name text
);

GRANT SELECT (name)
   ON TABLE public.test4
   TO auditor;

GRANT UPDATE (id)
   ON TABLE public.test4
   TO auditor;

GRANT insert (name)
   ON TABLE public.test4
   TO auditor;

--
-- Not object logged
SELECT id
  FROM public.test4;

--
-- Object logged because of:
-- select (name) on test4
SELECT name
  FROM public.test4;

--
-- Not object logged
INSERT INTO public.test4 (id)
				  VALUES (1);

--
-- Object logged because of:
-- insert (name) on test4
INSERT INTO public.test4 (name)
				  VALUES ('test');

--
-- Not object logged
UPDATE public.test4
   SET name = 'foo';

--
-- Object logged because of:
-- update (id) on test4
UPDATE public.test4
   SET id = 1;

--
-- Object logged because of:
-- update (name) on test4
-- update (name) takes precedence over select (name) due to ordering
update public.test4 set name = 'foo' where name = 'bar';

--
-- Drop test tables
DROP TABLE test2;
DROP VIEW vw_test3;
DROP TABLE test3;
DROP TABLE test4;

--
-- Change permissions of user 1 so that session logging will be done
\connect - :current_user
select load_pg_audit();
alter role user1 set pg_audit.log = 'DDL, READ';
\connect - user1
select load_pg_audit();

--
-- Create table is session logged
CREATE TABLE public.account
(
	id INT,
	name TEXT,
	password TEXT,
	description TEXT
);

--
-- Select is session logged
SELECT *
  FROM account;

--
-- Insert is not logged
INSERT INTO account (id, name, password, description)
			 VALUES (1, 'user1', 'HASH1', 'blah, blah');

--
-- Change permissions of user 1 so that only object logging will be done
\connect - :current_user
select load_pg_audit();
alter role user1 set pg_audit.log = 'none';
alter role user1 set pg_audit.role = 'auditor';
\connect - user1
select load_pg_audit();

--
-- ROLE class not set, so auditor grants not logged
GRANT SELECT (password),
	  UPDATE (name, password)
   ON TABLE public.account
   TO auditor;

--
-- Not object logged
SELECT id,
	   name
  FROM account;

--
-- Object logged because of:
-- select (password) on account
SELECT password
  FROM account;

--
-- Not object logged
UPDATE account
   SET description = 'yada, yada';

--
-- Object logged because of:
-- update (password) on account
UPDATE account
   SET password = 'HASH2';

--
-- Change permissions of user 1 so that session relation logging will be done
\connect - :current_user
select load_pg_audit();
alter role user1 set pg_audit.log_relation = on;
alter role user1 set pg_audit.log = 'read, WRITE';
\connect - user1
select load_pg_audit();

--
-- Not logged
create table ACCOUNT_ROLE_MAP
(
	account_id INT,
	role_id INT
);

--
-- ROLE class not set, so auditor grants not logged
GRANT SELECT
   ON TABLE public.account_role_map
   TO auditor;

--
-- Object logged because of:
-- select (password) on account
-- select on account_role_map
-- Session logged on all tables because log = read and log_relation = on
SELECT account.password,
	   account_role_map.role_id
  FROM account
	   INNER JOIN account_role_map
			on account.id = account_role_map.account_id;

--
-- Object logged because of:
-- select (password) on account
-- Session logged on all tables because log = read and log_relation = on
SELECT password
  FROM account;

--
-- Not object logged
-- Session logged on all tables because log = read and log_relation = on
UPDATE account
   SET description = 'yada, yada';

--
-- Object logged because of:
-- select (password) on account (in the where clause)
-- Session logged on all tables because log = read and log_relation = on
UPDATE account
   SET description = 'yada, yada'
 where password = 'HASH2';

--
-- Object logged because of:
-- update (password) on account
-- Session logged on all tables because log = read and log_relation = on
UPDATE account
   SET password = 'HASH2';

--
-- Change back to superuser to do exhaustive tests
\connect - :current_user
select load_pg_audit();
SET pg_audit.log = 'ALL';
SET pg_audit.log_level = 'notice';
SET pg_audit.log_relation = ON;
SET pg_audit.log_parameter = ON;

--
-- Simple DO block
DO $$
BEGIN
	raise notice 'test';
END $$;

--
-- Create test schema
CREATE SCHEMA test;

--
-- Copy account to stdout
COPY account TO stdout;

--
-- Create a table from a query
CREATE TABLE test.account_copy AS
SELECT *
  FROM account;

--
-- Copy from stdin to account copy
COPY test.account_copy from stdin;
1	user1	HASH2	yada, yada
\.

--
-- Test prepared statement
PREPARE pgclassstmt (oid) AS
SELECT *
  FROM account
 WHERE id = $1;

EXECUTE pgclassstmt (1);
DEALLOCATE pgclassstmt;

--
-- Test cursor
BEGIN;

DECLARE ctest SCROLL CURSOR FOR
SELECT count(*)
  FROM
(
	SELECT relname
	  FROM pg_class
	 LIMIT 1
 ) subquery;

FETCH NEXT FROM ctest;
CLOSE ctest;
COMMIT;

--
-- Turn off log_catalog and pg_class will not be logged
SET pg_audit.log_catalog = OFF;

SELECT count(*)
  FROM
(
	SELECT relname
	  FROM pg_class
	 LIMIT 1
 ) subquery;

--
-- Test prepared insert
CREATE TABLE test.test_insert
(
	id INT
);

PREPARE pgclassstmt (oid) AS
INSERT INTO test.test_insert (id)
					  VALUES ($1);
EXECUTE pgclassstmt (1);

--
-- Check that primary key creation is logged
CREATE TABLE public.test
(
	id INT,
	name TEXT,
	description TEXT,
	CONSTRAINT test_pkey PRIMARY KEY (id)
);

--
-- Check that analyze is logged
ANALYZE test;

--
-- Grants to public should not cause object logging (session logging will
-- still happen)
GRANT SELECT
  ON TABLE public.test
  TO PUBLIC;

SELECT *
  FROM test;

-- Check that statements without columns log
SELECT
  FROM test;

SELECT 1,
	   substring('Thomas' from 2 for 3);

DO $$
DECLARE
	test INT;
BEGIN
	SELECT 1
	  INTO test;
END $$;

explain select 1;

--
-- Test that looks inside of do blocks log
INSERT INTO TEST (id)
		  VALUES (1);
INSERT INTO TEST (id)
		  VALUES (2);
INSERT INTO TEST (id)
		  VALUES (3);

DO $$
DECLARE
	result RECORD;
BEGIN
	FOR result IN
		SELECT id
		  FROM test
	LOOP
		INSERT INTO test (id)
			 VALUES (result.id + 100);
	END LOOP;
END $$;

--
-- Test obfuscated dynamic sql for clean logging
DO $$
DECLARE
	table_name TEXT = 'do_table';
BEGIN
	EXECUTE 'CREATE TABLE ' || table_name || ' ("weird name" INT)';
	EXECUTE 'DROP table ' || table_name;
END $$;

--
-- Generate an error and make sure the stack gets cleared
DO $$
BEGIN
	CREATE TABLE bogus.test_block
	(
		id INT
	);
END $$;

--
-- Test alter table statements
ALTER TABLE public.test
	DROP COLUMN description ;

ALTER TABLE public.test
	RENAME TO test2;

ALTER TABLE public.test2
	SET SCHEMA test;

ALTER TABLE test.test2
	ADD COLUMN description TEXT;

ALTER TABLE test.test2
	DROP COLUMN description;

DROP TABLE test.test2;

--
-- Test multiple statements with one semi-colon
CREATE SCHEMA foo
	CREATE TABLE foo.bar (id int)
	CREATE TABLE foo.baz (id int);

--
-- Test aggregate
CREATE FUNCTION public.int_add
(
	a INT,
	b INT
)
	RETURNS INT LANGUAGE plpgsql AS $$
BEGIN
	return a + b;
END $$;

SELECT int_add(1, 1);

CREATE AGGREGATE public.sum_test(INT) (SFUNC=public.int_add, STYPE=INT, INITCOND='0');
ALTER AGGREGATE public.sum_test(integer) RENAME TO sum_test2;

--
-- Test conversion
CREATE CONVERSION public.conversion_test FOR 'SQL_ASCII' TO 'MULE_INTERNAL' FROM pg_catalog.ascii_to_mic;
ALTER CONVERSION public.conversion_test RENAME TO conversion_test2;

--
-- Test create/alter/drop database
CREATE DATABASE contrib_regression_pgaudit;
ALTER DATABASE contrib_regression_pgaudit RENAME TO contrib_regression_pgaudit2;
DROP DATABASE contrib_regression_pgaudit2;

--
-- Test that frees a memory context earlier than expected
CREATE TABLE hoge
(
	id int
);

CREATE FUNCTION test()
	RETURNS INT AS $$
DECLARE
	cur1 cursor for select * from hoge;
	tmp int;
BEGIN
	OPEN cur1;
	FETCH cur1 into tmp;
	RETURN tmp;
END $$
LANGUAGE plpgsql ;

SELECT test();

--
-- Delete all rows then delete 1 row
SET pg_audit.log = 'write';
SET pg_audit.role = 'auditor';

create table bar
(
	col int
);

grant delete
   on bar
   to auditor;

insert into bar (col)
		 values (1);
delete from bar;

insert into bar (col)
		 values (1);
delete from bar
 where col = 1;

drop table bar;

--
-- Grant roles to each other
SET pg_audit.log = 'role';
GRANT user1 TO user2;
REVOKE user1 FROM user2;

DROP TABLE test.account_copy;
DROP TABLE test.test_insert;
DROP SCHEMA test;
DROP TABLE foo.bar;
DROP TABLE foo.baz;
DROP SCHEMA foo;
DROP TABLE hoge;
DROP TABLE account;
DROP TABLE account_role_map;
DROP USER user2;
DROP USER user1;
DROP ROLE auditor;
