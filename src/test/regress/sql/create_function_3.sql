--
-- CREATE FUNCTION
--
-- sanity check of pg_proc catalog to the given parameters
--
CREATE USER regtest_unpriv_user;

CREATE SCHEMA temp_func_test;
GRANT ALL ON SCHEMA temp_func_test TO public;

SET search_path TO temp_func_test, public;

--
-- ARGUMENT and RETURN TYPES
--
CREATE FUNCTION functest_A_1(text, date) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 = ''abcd'' AND $2 > ''2001-01-01''';
CREATE FUNCTION functest_A_2(text[]) RETURNS int LANGUAGE 'sql'
       AS 'SELECT $1[0]::int';
CREATE FUNCTION functest_A_3() RETURNS bool LANGUAGE 'sql'
       AS 'SELECT false';
SELECT proname, prorettype::regtype, proargtypes::regtype[] FROM pg_proc
       WHERE oid in ('functest_A_1'::regproc,
                     'functest_A_2'::regproc,
                     'functest_A_3'::regproc) ORDER BY proname;

--
-- IMMUTABLE | STABLE | VOLATILE
--
CREATE FUNCTION functest_B_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 0';
CREATE FUNCTION functest_B_2(int) RETURNS bool LANGUAGE 'sql'
       IMMUTABLE AS 'SELECT $1 > 0';
CREATE FUNCTION functest_B_3(int) RETURNS bool LANGUAGE 'sql'
       STABLE AS 'SELECT $1 = 0';
CREATE FUNCTION functest_B_4(int) RETURNS bool LANGUAGE 'sql'
       VOLATILE AS 'SELECT $1 < 0';
SELECT proname, provolatile FROM pg_proc
       WHERE oid in ('functest_B_1'::regproc,
                     'functest_B_2'::regproc,
                     'functest_B_3'::regproc,
		     'functest_B_4'::regproc) ORDER BY proname;

ALTER FUNCTION functest_B_2(int) VOLATILE;
ALTER FUNCTION functest_B_3(int) COST 100;	-- unrelated change, no effect
SELECT proname, provolatile FROM pg_proc
       WHERE oid in ('functest_B_1'::regproc,
                     'functest_B_2'::regproc,
                     'functest_B_3'::regproc,
		     'functest_B_4'::regproc) ORDER BY proname;

--
-- SECURITY DEFINER | INVOKER
--
CREATE FUNCTION functext_C_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 0';
CREATE FUNCTION functext_C_2(int) RETURNS bool LANGUAGE 'sql'
       SECURITY DEFINER AS 'SELECT $1 = 0';
CREATE FUNCTION functext_C_3(int) RETURNS bool LANGUAGE 'sql'
       SECURITY INVOKER AS 'SELECT $1 < 0';
SELECT proname, prosecdef FROM pg_proc
       WHERE oid in ('functext_C_1'::regproc,
                     'functext_C_2'::regproc,
                     'functext_C_3'::regproc) ORDER BY proname;

ALTER FUNCTION functext_C_1(int) IMMUTABLE;	-- unrelated change, no effect
ALTER FUNCTION functext_C_2(int) SECURITY INVOKER;
ALTER FUNCTION functext_C_3(int) SECURITY DEFINER;
SELECT proname, prosecdef FROM pg_proc
       WHERE oid in ('functext_C_1'::regproc,
                     'functext_C_2'::regproc,
                     'functext_C_3'::regproc) ORDER BY proname;

--
-- LEAKPROOF
--
CREATE FUNCTION functext_E_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 100';
CREATE FUNCTION functext_E_2(int) RETURNS bool LANGUAGE 'sql'
       LEAKPROOF AS 'SELECT $1 > 100';
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functext_E_1'::regproc,
                     'functext_E_2'::regproc) ORDER BY proname;

ALTER FUNCTION functext_E_1(int) LEAKPROOF;
ALTER FUNCTION functext_E_2(int) STABLE;	-- unrelated change, no effect
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functext_E_1'::regproc,
                     'functext_E_2'::regproc) ORDER BY proname;

ALTER FUNCTION functext_E_2(int) NOT LEAKPROOF;	-- remove leakproog attribute
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functext_E_1'::regproc,
                     'functext_E_2'::regproc) ORDER BY proname;

-- it takes superuser privilege to turn on leakproof, but not for turn off
ALTER FUNCTION functext_E_1(int) OWNER TO regtest_unpriv_user;
ALTER FUNCTION functext_E_2(int) OWNER TO regtest_unpriv_user;

SET SESSION AUTHORIZATION regtest_unpriv_user;
SET search_path TO temp_func_test, public;
ALTER FUNCTION functext_E_1(int) NOT LEAKPROOF;
ALTER FUNCTION functext_E_2(int) LEAKPROOF;

CREATE FUNCTION functext_E_3(int) RETURNS bool LANGUAGE 'sql'
       LEAKPROOF AS 'SELECT $1 < 200';	-- failed

RESET SESSION AUTHORIZATION;

-- list of built-in leakproof functions
SELECT proname, prorettype::regtype, proargtypes::regtype[]
       FROM pg_proc JOIN pg_namespace ON pronamespace = pg_namespace.oid
       WHERE nspname = 'pg_catalog' AND proleakproof ORDER BY proname;

--
-- CALLED ON NULL INPUT | RETURNS NULL ON NULL INPUT | STRICT
--
CREATE FUNCTION functext_F_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 50';
CREATE FUNCTION functext_F_2(int) RETURNS bool LANGUAGE 'sql'
       CALLED ON NULL INPUT AS 'SELECT $1 = 50';
CREATE FUNCTION functext_F_3(int) RETURNS bool LANGUAGE 'sql'
       RETURNS NULL ON NULL INPUT AS 'SELECT $1 < 50';
CREATE FUNCTION functext_F_4(int) RETURNS bool LANGUAGE 'sql'
       STRICT AS 'SELECT $1 = 50';
SELECT proname, proisstrict FROM pg_proc
       WHERE oid in ('functext_F_1'::regproc,
                     'functext_F_2'::regproc,
                     'functext_F_3'::regproc,
                     'functext_F_4'::regproc) ORDER BY proname;

ALTER FUNCTION functext_F_1(int) IMMUTABLE;	-- unrelated change, no effect
ALTER FUNCTION functext_F_2(int) STRICT;
ALTER FUNCTION functext_F_3(int) CALLED ON NULL INPUT;
SELECT proname, proisstrict FROM pg_proc
       WHERE oid in ('functext_F_1'::regproc,
                     'functext_F_2'::regproc,
                     'functext_F_3'::regproc,
                     'functext_F_4'::regproc) ORDER BY proname;

-- Cleanups
DROP SCHEMA temp_func_test CASCADE;
DROP USER regtest_unpriv_user;
RESET search_path;
