CREATE DATABASE regression_tbd
	ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;
ALTER DATABASE regression_tbd RENAME TO regression_utf8;
ALTER DATABASE regression_utf8 SET TABLESPACE regress_tblspace;
ALTER DATABASE regression_utf8 SET TABLESPACE pg_default;
ALTER DATABASE regression_utf8 CONNECTION_LIMIT 123;

-- Test PgDatabaseToastTable.  Doing this with GRANT would be slow.
BEGIN;
UPDATE pg_database
SET datacl = array_fill(makeaclitem(10, 10, 'USAGE', false), ARRAY[5e5::int])
WHERE datname = 'regression_utf8';
-- load catcache entry, if nothing else does
ALTER DATABASE regression_utf8 RENAME TO regression_rename_rolled_back;
ROLLBACK;

CREATE ROLE regress_datdba_before;
CREATE ROLE regress_datdba_after;
ALTER DATABASE regression_utf8 OWNER TO regress_datdba_before;
REASSIGN OWNED BY regress_datdba_before TO regress_datdba_after;

DROP DATABASE regression_utf8;
DROP ROLE regress_datdba_before;
DROP ROLE regress_datdba_after;

-- Create a specific role to test
CREATE ROLE regress_ddl_database WITH SUPERUSER;
CREATE DATABASE "regression_get_database_ddl"
    OWNER regress_ddl_database ENCODING 'UTF8' LC_COLLATE "C" LC_CTYPE "C"
    TEMPLATE template0;
-- Test LOCAL_PROVIDER and BUILTIN_LOCALE for builtin type
CREATE DATABASE "regression_get_database_ddl_builtin"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    BUILTIN_LOCALE 'C.UTF-8' LOCALE_PROVIDER 'builtin';
-- Test ALLOW_CONNECTION and CONNECTION_LIMIT
CREATE DATABASE "regression_get_database_ddl_conn"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    ALLOW_CONNECTIONS 0 CONNECTION LIMIT 50;

-- Database doesn't exists
SELECT pg_get_database_ddl('regression_database', false);

-- Test NULL value
SELECT pg_get_database_ddl(NULL);

-- Without Pretty formatted
SELECT pg_get_database_ddl('regression_get_database_ddl');
SELECT pg_get_database_ddl('regression_get_database_ddl_builtin');
SELECT pg_get_database_ddl('regression_get_database_ddl_conn', false);

-- With Pretty formatted
\pset format unaligned
SELECT pg_get_database_ddl('regression_get_database_ddl', true);
SELECT pg_get_database_ddl('regression_get_database_ddl_builtin', true);
SELECT pg_get_database_ddl('regression_get_database_ddl_conn', true);

-- Clean up
DROP DATABASE "regression_get_database_ddl";
DROP DATABASE "regression_get_database_ddl_builtin";
DROP DATABASE "regression_get_database_ddl_conn";
DROP ROLE regress_ddl_database;