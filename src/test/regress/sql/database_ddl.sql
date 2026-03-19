--
-- Tests for pg_get_database_ddl()
--

-- To produce stable regression test output, strip locale/collation details
-- from the DDL output.  Uses a plain SQL function to avoid a PL/pgSQL
-- dependency.

CREATE OR REPLACE FUNCTION ddl_filter(ddl_input TEXT)
RETURNS TEXT LANGUAGE sql AS $$
SELECT regexp_replace(
  regexp_replace(
    regexp_replace(
      regexp_replace(
        regexp_replace(
          ddl_input,
          '\s*\mLOCALE_PROVIDER\M\s*=\s*([''"]?[^''"\s]+[''"]?)', '', 'gi'),
        '\s*LC_COLLATE\s*=\s*([''"])[^''"]*\1', '', 'gi'),
      '\s*LC_CTYPE\s*=\s*([''"])[^''"]*\1', '', 'gi'),
    '\s*\S*LOCALE\S*\s*=?\s*([''"])[^''"]*\1', '', 'gi'),
  '\s*\S*COLLATION\S*\s*=?\s*([''"])[^''"]*\1', '', 'gi')
$$;

CREATE ROLE regress_datdba;
CREATE DATABASE regress_database_ddl
    ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0
    OWNER regress_datdba;
ALTER DATABASE regress_database_ddl CONNECTION_LIMIT 123;
ALTER DATABASE regress_database_ddl SET random_page_cost = 2.0;
ALTER ROLE regress_datdba IN DATABASE regress_database_ddl SET random_page_cost = 1.1;

-- Database doesn't exist
SELECT * FROM pg_get_database_ddl('regression_database');

-- NULL value
SELECT * FROM pg_get_database_ddl(NULL);

-- Invalid option value (should error)
SELECT * FROM pg_get_database_ddl('regress_database_ddl', 'owner', 'invalid');

-- Duplicate option (should error)
SELECT * FROM pg_get_database_ddl('regress_database_ddl', 'owner', 'false', 'owner', 'true');

-- Without options
SELECT ddl_filter(pg_get_database_ddl) FROM pg_get_database_ddl('regress_database_ddl');

-- With owner
SELECT ddl_filter(pg_get_database_ddl) FROM pg_get_database_ddl('regress_database_ddl', 'owner', 'true');

-- Pretty-printed output
\pset format unaligned
SELECT ddl_filter(pg_get_database_ddl) FROM pg_get_database_ddl('regress_database_ddl', 'pretty', 'true', 'tablespace', 'false');
\pset format aligned

-- Permission check: revoke CONNECT on database
CREATE ROLE regress_db_ddl_noaccess;
REVOKE CONNECT ON DATABASE regress_database_ddl FROM PUBLIC;
SET ROLE regress_db_ddl_noaccess;
SELECT * FROM pg_get_database_ddl('regress_database_ddl');  -- should fail
RESET ROLE;
GRANT CONNECT ON DATABASE regress_database_ddl TO PUBLIC;
DROP ROLE regress_db_ddl_noaccess;

DROP DATABASE regress_database_ddl;
DROP FUNCTION ddl_filter(text);
DROP ROLE regress_datdba;
