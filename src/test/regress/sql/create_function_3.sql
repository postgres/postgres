--
-- CREATE FUNCTION
--
-- Assorted tests using SQL-language functions
--

-- All objects made in this test are in temp_func_test schema

CREATE USER regress_unpriv_user;

CREATE SCHEMA temp_func_test;
GRANT ALL ON SCHEMA temp_func_test TO public;

SET search_path TO temp_func_test, public;

--
-- Make sanity checks on the pg_proc entries created by CREATE FUNCTION
--

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
CREATE FUNCTION functest_C_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 0';
CREATE FUNCTION functest_C_2(int) RETURNS bool LANGUAGE 'sql'
       SECURITY DEFINER AS 'SELECT $1 = 0';
CREATE FUNCTION functest_C_3(int) RETURNS bool LANGUAGE 'sql'
       SECURITY INVOKER AS 'SELECT $1 < 0';
SELECT proname, prosecdef FROM pg_proc
       WHERE oid in ('functest_C_1'::regproc,
                     'functest_C_2'::regproc,
                     'functest_C_3'::regproc) ORDER BY proname;

ALTER FUNCTION functest_C_1(int) IMMUTABLE;	-- unrelated change, no effect
ALTER FUNCTION functest_C_2(int) SECURITY INVOKER;
ALTER FUNCTION functest_C_3(int) SECURITY DEFINER;
SELECT proname, prosecdef FROM pg_proc
       WHERE oid in ('functest_C_1'::regproc,
                     'functest_C_2'::regproc,
                     'functest_C_3'::regproc) ORDER BY proname;

--
-- LEAKPROOF
--
CREATE FUNCTION functest_E_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 100';
CREATE FUNCTION functest_E_2(int) RETURNS bool LANGUAGE 'sql'
       LEAKPROOF AS 'SELECT $1 > 100';
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functest_E_1'::regproc,
                     'functest_E_2'::regproc) ORDER BY proname;

ALTER FUNCTION functest_E_1(int) LEAKPROOF;
ALTER FUNCTION functest_E_2(int) STABLE;	-- unrelated change, no effect
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functest_E_1'::regproc,
                     'functest_E_2'::regproc) ORDER BY proname;

ALTER FUNCTION functest_E_2(int) NOT LEAKPROOF;	-- remove leakproof attribute
SELECT proname, proleakproof FROM pg_proc
       WHERE oid in ('functest_E_1'::regproc,
                     'functest_E_2'::regproc) ORDER BY proname;

-- it takes superuser privilege to turn on leakproof, but not to turn off
ALTER FUNCTION functest_E_1(int) OWNER TO regress_unpriv_user;
ALTER FUNCTION functest_E_2(int) OWNER TO regress_unpriv_user;

SET SESSION AUTHORIZATION regress_unpriv_user;
SET search_path TO temp_func_test, public;
ALTER FUNCTION functest_E_1(int) NOT LEAKPROOF;
ALTER FUNCTION functest_E_2(int) LEAKPROOF;

CREATE FUNCTION functest_E_3(int) RETURNS bool LANGUAGE 'sql'
       LEAKPROOF AS 'SELECT $1 < 200';	-- fail

RESET SESSION AUTHORIZATION;

--
-- CALLED ON NULL INPUT | RETURNS NULL ON NULL INPUT | STRICT
--
CREATE FUNCTION functest_F_1(int) RETURNS bool LANGUAGE 'sql'
       AS 'SELECT $1 > 50';
CREATE FUNCTION functest_F_2(int) RETURNS bool LANGUAGE 'sql'
       CALLED ON NULL INPUT AS 'SELECT $1 = 50';
CREATE FUNCTION functest_F_3(int) RETURNS bool LANGUAGE 'sql'
       RETURNS NULL ON NULL INPUT AS 'SELECT $1 < 50';
CREATE FUNCTION functest_F_4(int) RETURNS bool LANGUAGE 'sql'
       STRICT AS 'SELECT $1 = 50';
SELECT proname, proisstrict FROM pg_proc
       WHERE oid in ('functest_F_1'::regproc,
                     'functest_F_2'::regproc,
                     'functest_F_3'::regproc,
                     'functest_F_4'::regproc) ORDER BY proname;

ALTER FUNCTION functest_F_1(int) IMMUTABLE;	-- unrelated change, no effect
ALTER FUNCTION functest_F_2(int) STRICT;
ALTER FUNCTION functest_F_3(int) CALLED ON NULL INPUT;
SELECT proname, proisstrict FROM pg_proc
       WHERE oid in ('functest_F_1'::regproc,
                     'functest_F_2'::regproc,
                     'functest_F_3'::regproc,
                     'functest_F_4'::regproc) ORDER BY proname;


-- pg_get_functiondef tests

SELECT pg_get_functiondef('functest_A_1'::regproc);
SELECT pg_get_functiondef('functest_B_3'::regproc);
SELECT pg_get_functiondef('functest_C_3'::regproc);
SELECT pg_get_functiondef('functest_F_2'::regproc);


-- information_schema tests

CREATE FUNCTION functest_IS_1(a int, b int default 1, c text default 'foo')
    RETURNS int
    LANGUAGE SQL
    AS 'SELECT $1 + $2';

CREATE FUNCTION functest_IS_2(out a int, b int default 1)
    RETURNS int
    LANGUAGE SQL
    AS 'SELECT $1';

CREATE FUNCTION functest_IS_3(a int default 1, out b int)
    RETURNS int
    LANGUAGE SQL
    AS 'SELECT $1';

SELECT routine_name, ordinal_position, parameter_name, parameter_default
    FROM information_schema.parameters JOIN information_schema.routines USING (specific_schema, specific_name)
    WHERE routine_schema = 'temp_func_test' AND routine_name ~ '^functest_is_'
    ORDER BY 1, 2;

DROP FUNCTION functest_IS_1(int, int, text), functest_IS_2(int), functest_IS_3(int);

-- overload
CREATE FUNCTION functest_B_2(bigint) RETURNS bool LANGUAGE 'sql'
       IMMUTABLE AS 'SELECT $1 > 0';

DROP FUNCTION functest_b_1;
DROP FUNCTION functest_b_1;  -- error, not found
DROP FUNCTION functest_b_2;  -- error, ambiguous


-- CREATE OR REPLACE tests

CREATE FUNCTION functest1(a int) RETURNS int LANGUAGE SQL AS 'SELECT $1';
CREATE OR REPLACE FUNCTION functest1(a int) RETURNS int LANGUAGE SQL WINDOW AS 'SELECT $1';
CREATE OR REPLACE PROCEDURE functest1(a int) LANGUAGE SQL AS 'SELECT $1';
DROP FUNCTION functest1(a int);


-- Check behavior of VOID-returning SQL functions

CREATE FUNCTION voidtest1(a int) RETURNS VOID LANGUAGE SQL AS
$$ SELECT a + 1 $$;
SELECT voidtest1(42);

CREATE FUNCTION voidtest2(a int, b int) RETURNS VOID LANGUAGE SQL AS
$$ SELECT voidtest1(a + b) $$;
SELECT voidtest2(11,22);

-- currently, we can inline voidtest2 but not voidtest1
EXPLAIN (verbose, costs off) SELECT voidtest2(11,22);

CREATE TEMP TABLE sometable(f1 int);

CREATE FUNCTION voidtest3(a int) RETURNS VOID LANGUAGE SQL AS
$$ INSERT INTO sometable VALUES(a + 1) $$;
SELECT voidtest3(17);

CREATE FUNCTION voidtest4(a int) RETURNS VOID LANGUAGE SQL AS
$$ INSERT INTO sometable VALUES(a - 1) RETURNING f1 $$;
SELECT voidtest4(39);

TABLE sometable;

CREATE FUNCTION voidtest5(a int) RETURNS SETOF VOID LANGUAGE SQL AS
$$ SELECT generate_series(1, a) $$ STABLE;
SELECT * FROM voidtest5(3);

-- Regression tests for bugs:

-- Check that arguments that are R/W expanded datums aren't corrupted by
-- multiple uses.  This test knows that array_append() returns a R/W datum
-- and will modify a R/W array input in-place.  We use SETOF to prevent
-- inlining of the SQL function.
CREATE FUNCTION double_append(anyarray, anyelement) RETURNS SETOF anyarray
LANGUAGE SQL IMMUTABLE AS
$$ SELECT array_append($1, $2) || array_append($1, $2) $$;

SELECT double_append(array_append(ARRAY[q1], q2), q3)
  FROM (VALUES(1,2,3), (4,5,6)) v(q1,q2,q3);

-- Cleanup
DROP SCHEMA temp_func_test CASCADE;
DROP USER regress_unpriv_user;
RESET search_path;
