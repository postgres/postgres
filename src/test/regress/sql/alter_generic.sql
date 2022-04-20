--
-- Test for ALTER some_object {RENAME TO, OWNER TO, SET SCHEMA}
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_opclass_options_func(internal)
    RETURNS void
    AS :'regresslib', 'test_opclass_options_func'
    LANGUAGE C;

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS regress_alter_generic_user1;
DROP ROLE IF EXISTS regress_alter_generic_user2;
DROP ROLE IF EXISTS regress_alter_generic_user3;

RESET client_min_messages;

CREATE USER regress_alter_generic_user3;
CREATE USER regress_alter_generic_user2;
CREATE USER regress_alter_generic_user1 IN ROLE regress_alter_generic_user3;

CREATE SCHEMA alt_nsp1;
CREATE SCHEMA alt_nsp2;

GRANT ALL ON SCHEMA alt_nsp1, alt_nsp2 TO public;

SET search_path = alt_nsp1, public;

--
-- Function and Aggregate
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;
CREATE FUNCTION alt_func1(int) RETURNS int LANGUAGE sql
  AS 'SELECT $1 + 1';
CREATE FUNCTION alt_func2(int) RETURNS int LANGUAGE sql
  AS 'SELECT $1 - 1';
CREATE AGGREGATE alt_agg1 (
  sfunc1 = int4pl, basetype = int4, stype1 = int4, initcond = 0
);
CREATE AGGREGATE alt_agg2 (
  sfunc1 = int4mi, basetype = int4, stype1 = int4, initcond = 0
);
ALTER AGGREGATE alt_func1(int) RENAME TO alt_func3;  -- failed (not aggregate)
ALTER AGGREGATE alt_func1(int) OWNER TO regress_alter_generic_user3;  -- failed (not aggregate)
ALTER AGGREGATE alt_func1(int) SET SCHEMA alt_nsp2;  -- failed (not aggregate)

ALTER FUNCTION alt_func1(int) RENAME TO alt_func2;  -- failed (name conflict)
ALTER FUNCTION alt_func1(int) RENAME TO alt_func3;  -- OK
ALTER FUNCTION alt_func2(int) OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER FUNCTION alt_func2(int) OWNER TO regress_alter_generic_user3;  -- OK
ALTER FUNCTION alt_func2(int) SET SCHEMA alt_nsp1;  -- OK, already there
ALTER FUNCTION alt_func2(int) SET SCHEMA alt_nsp2;  -- OK

ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg2;   -- failed (name conflict)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg3;   -- OK
ALTER AGGREGATE alt_agg2(int) OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER AGGREGATE alt_agg2(int) OWNER TO regress_alter_generic_user3;  -- OK
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;
CREATE FUNCTION alt_func1(int) RETURNS int LANGUAGE sql
  AS 'SELECT $1 + 2';
CREATE FUNCTION alt_func2(int) RETURNS int LANGUAGE sql
  AS 'SELECT $1 - 2';
CREATE AGGREGATE alt_agg1 (
  sfunc1 = int4pl, basetype = int4, stype1 = int4, initcond = 100
);
CREATE AGGREGATE alt_agg2 (
  sfunc1 = int4mi, basetype = int4, stype1 = int4, initcond = -100
);

ALTER FUNCTION alt_func3(int) RENAME TO alt_func4;	-- failed (not owner)
ALTER FUNCTION alt_func1(int) RENAME TO alt_func4;	-- OK
ALTER FUNCTION alt_func3(int) OWNER TO regress_alter_generic_user2;	-- failed (not owner)
ALTER FUNCTION alt_func2(int) OWNER TO regress_alter_generic_user3;	-- failed (no role membership)
ALTER FUNCTION alt_func3(int) SET SCHEMA alt_nsp2;      -- failed (not owner)
ALTER FUNCTION alt_func2(int) SET SCHEMA alt_nsp2;	-- failed (name conflicts)

ALTER AGGREGATE alt_agg3(int) RENAME TO alt_agg4;   -- failed (not owner)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg4;   -- OK
ALTER AGGREGATE alt_agg3(int) OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER AGGREGATE alt_agg2(int) OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER AGGREGATE alt_agg3(int) SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT n.nspname, proname, prorettype::regtype, prokind, a.rolname
  FROM pg_proc p, pg_namespace n, pg_authid a
  WHERE p.pronamespace = n.oid AND p.proowner = a.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, proname;

--
-- We would test collations here, but it's not possible because the error
-- messages tend to be nonportable.
--

--
-- Conversion
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;
CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv1 RENAME TO alt_conv2;  -- failed (name conflict)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv3;  -- OK
ALTER CONVERSION alt_conv2 OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER CONVERSION alt_conv2 OWNER TO regress_alter_generic_user3;  -- OK
ALTER CONVERSION alt_conv2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;
CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv3 RENAME TO alt_conv4;  -- failed (not owner)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv4;  -- OK
ALTER CONVERSION alt_conv3 OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER CONVERSION alt_conv2 OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER CONVERSION alt_conv3 SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER CONVERSION alt_conv2 SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT n.nspname, c.conname, a.rolname
  FROM pg_conversion c, pg_namespace n, pg_authid a
  WHERE c.connamespace = n.oid AND c.conowner = a.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, conname;

--
-- Foreign Data Wrapper and Foreign Server
--
CREATE FOREIGN DATA WRAPPER alt_fdw1;
CREATE FOREIGN DATA WRAPPER alt_fdw2;

CREATE SERVER alt_fserv1 FOREIGN DATA WRAPPER alt_fdw1;
CREATE SERVER alt_fserv2 FOREIGN DATA WRAPPER alt_fdw2;

ALTER FOREIGN DATA WRAPPER alt_fdw1 RENAME TO alt_fdw2;  -- failed (name conflict)
ALTER FOREIGN DATA WRAPPER alt_fdw1 RENAME TO alt_fdw3;  -- OK

ALTER SERVER alt_fserv1 RENAME TO alt_fserv2;   -- failed (name conflict)
ALTER SERVER alt_fserv1 RENAME TO alt_fserv3;   -- OK

SELECT fdwname FROM pg_foreign_data_wrapper WHERE fdwname like 'alt_fdw%';
SELECT srvname FROM pg_foreign_server WHERE srvname like 'alt_fserv%';

--
-- Procedural Language
--
CREATE LANGUAGE alt_lang1 HANDLER plpgsql_call_handler;
CREATE LANGUAGE alt_lang2 HANDLER plpgsql_call_handler;

ALTER LANGUAGE alt_lang1 OWNER TO regress_alter_generic_user1;  -- OK
ALTER LANGUAGE alt_lang2 OWNER TO regress_alter_generic_user2;  -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user1;
ALTER LANGUAGE alt_lang1 RENAME TO alt_lang2;   -- failed (name conflict)
ALTER LANGUAGE alt_lang2 RENAME TO alt_lang3;   -- failed (not owner)
ALTER LANGUAGE alt_lang1 RENAME TO alt_lang3;   -- OK

ALTER LANGUAGE alt_lang2 OWNER TO regress_alter_generic_user3;  -- failed (not owner)
ALTER LANGUAGE alt_lang3 OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER LANGUAGE alt_lang3 OWNER TO regress_alter_generic_user3;  -- OK

RESET SESSION AUTHORIZATION;
SELECT lanname, a.rolname
  FROM pg_language l, pg_authid a
  WHERE l.lanowner = a.oid AND l.lanname like 'alt_lang%'
  ORDER BY lanname;

--
-- Operator
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;

CREATE OPERATOR @-@ ( leftarg = int4, rightarg = int4, procedure = int4mi );
CREATE OPERATOR @+@ ( leftarg = int4, rightarg = int4, procedure = int4pl );

ALTER OPERATOR @+@(int4, int4) OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER OPERATOR @+@(int4, int4) OWNER TO regress_alter_generic_user3;  -- OK
ALTER OPERATOR @-@(int4, int4) SET SCHEMA alt_nsp2;           -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;

CREATE OPERATOR @-@ ( leftarg = int4, rightarg = int4, procedure = int4mi );

ALTER OPERATOR @+@(int4, int4) OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER OPERATOR @-@(int4, int4) OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER OPERATOR @+@(int4, int4) SET SCHEMA alt_nsp2;   -- failed (not owner)
-- can't test this: the error message includes the raw oid of namespace
-- ALTER OPERATOR @-@(int4, int4) SET SCHEMA alt_nsp2;   -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT n.nspname, oprname, a.rolname,
    oprleft::regtype, oprright::regtype, oprcode::regproc
  FROM pg_operator o, pg_namespace n, pg_authid a
  WHERE o.oprnamespace = n.oid AND o.oprowner = a.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, oprname;

--
-- OpFamily and OpClass
--
CREATE OPERATOR FAMILY alt_opf1 USING hash;
CREATE OPERATOR FAMILY alt_opf2 USING hash;
ALTER OPERATOR FAMILY alt_opf1 USING hash OWNER TO regress_alter_generic_user1;
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regress_alter_generic_user1;

CREATE OPERATOR CLASS alt_opc1 FOR TYPE uuid USING hash AS STORAGE uuid;
CREATE OPERATOR CLASS alt_opc2 FOR TYPE uuid USING hash AS STORAGE uuid;
ALTER OPERATOR CLASS alt_opc1 USING hash OWNER TO regress_alter_generic_user1;
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regress_alter_generic_user1;

SET SESSION AUTHORIZATION regress_alter_generic_user1;

ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf2;  -- failed (name conflict)
ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf3;  -- OK
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regress_alter_generic_user3;  -- OK
ALTER OPERATOR FAMILY alt_opf2 USING hash SET SCHEMA alt_nsp2;  -- OK

ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc2;  -- failed (name conflict)
ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc3;  -- OK
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regress_alter_generic_user3;  -- OK
ALTER OPERATOR CLASS alt_opc2 USING hash SET SCHEMA alt_nsp2;  -- OK

RESET SESSION AUTHORIZATION;

CREATE OPERATOR FAMILY alt_opf1 USING hash;
CREATE OPERATOR FAMILY alt_opf2 USING hash;
ALTER OPERATOR FAMILY alt_opf1 USING hash OWNER TO regress_alter_generic_user2;
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regress_alter_generic_user2;

CREATE OPERATOR CLASS alt_opc1 FOR TYPE macaddr USING hash AS STORAGE macaddr;
CREATE OPERATOR CLASS alt_opc2 FOR TYPE macaddr USING hash AS STORAGE macaddr;
ALTER OPERATOR CLASS alt_opc1 USING hash OWNER TO regress_alter_generic_user2;
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regress_alter_generic_user2;

SET SESSION AUTHORIZATION regress_alter_generic_user2;

ALTER OPERATOR FAMILY alt_opf3 USING hash RENAME TO alt_opf4;	-- failed (not owner)
ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf4;  -- OK
ALTER OPERATOR FAMILY alt_opf3 USING hash OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER OPERATOR FAMILY alt_opf3 USING hash SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER OPERATOR FAMILY alt_opf2 USING hash SET SCHEMA alt_nsp2;  -- failed (name conflict)

ALTER OPERATOR CLASS alt_opc3 USING hash RENAME TO alt_opc4;	-- failed (not owner)
ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc4;  -- OK
ALTER OPERATOR CLASS alt_opc3 USING hash OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER OPERATOR CLASS alt_opc3 USING hash SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER OPERATOR CLASS alt_opc2 USING hash SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT nspname, opfname, amname, rolname
  FROM pg_opfamily o, pg_am m, pg_namespace n, pg_authid a
  WHERE o.opfmethod = m.oid AND o.opfnamespace = n.oid AND o.opfowner = a.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
	AND NOT opfname LIKE 'alt_opc%'
  ORDER BY nspname, opfname;

SELECT nspname, opcname, amname, rolname
  FROM pg_opclass o, pg_am m, pg_namespace n, pg_authid a
  WHERE o.opcmethod = m.oid AND o.opcnamespace = n.oid AND o.opcowner = a.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, opcname;

-- ALTER OPERATOR FAMILY ... ADD/DROP

-- Should work. Textbook case of CREATE / ALTER ADD / ALTER DROP / DROP
BEGIN TRANSACTION;
CREATE OPERATOR FAMILY alt_opf4 USING btree;
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD
  -- int4 vs int2
  OPERATOR 1 < (int4, int2) ,
  OPERATOR 2 <= (int4, int2) ,
  OPERATOR 3 = (int4, int2) ,
  OPERATOR 4 >= (int4, int2) ,
  OPERATOR 5 > (int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2);

ALTER OPERATOR FAMILY alt_opf4 USING btree DROP
  -- int4 vs int2
  OPERATOR 1 (int4, int2) ,
  OPERATOR 2 (int4, int2) ,
  OPERATOR 3 (int4, int2) ,
  OPERATOR 4 (int4, int2) ,
  OPERATOR 5 (int4, int2) ,
  FUNCTION 1 (int4, int2) ;
DROP OPERATOR FAMILY alt_opf4 USING btree;
ROLLBACK;

-- Should fail. Invalid values for ALTER OPERATOR FAMILY .. ADD / DROP
CREATE OPERATOR FAMILY alt_opf4 USING btree;
ALTER OPERATOR FAMILY alt_opf4 USING invalid_index_method ADD  OPERATOR 1 < (int4, int2); -- invalid indexing_method
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD OPERATOR 6 < (int4, int2); -- operator number should be between 1 and 5
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD OPERATOR 0 < (int4, int2); -- operator number should be between 1 and 5
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD OPERATOR 1 < ; -- operator without argument types
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD FUNCTION 0 btint42cmp(int4, int2); -- invalid options parsing function
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD FUNCTION 6 btint42cmp(int4, int2); -- function number should be between 1 and 5
ALTER OPERATOR FAMILY alt_opf4 USING btree ADD STORAGE invalid_storage; -- Ensure STORAGE is not a part of ALTER OPERATOR FAMILY
DROP OPERATOR FAMILY alt_opf4 USING btree;

-- Should fail. Need to be SUPERUSER to do ALTER OPERATOR FAMILY .. ADD / DROP
BEGIN TRANSACTION;
CREATE ROLE regress_alter_generic_user5 NOSUPERUSER;
CREATE OPERATOR FAMILY alt_opf5 USING btree;
SET ROLE regress_alter_generic_user5;
ALTER OPERATOR FAMILY alt_opf5 USING btree ADD OPERATOR 1 < (int4, int2), FUNCTION 1 btint42cmp(int4, int2);
RESET ROLE;
DROP OPERATOR FAMILY alt_opf5 USING btree;
ROLLBACK;

-- Should fail. Need rights to namespace for ALTER OPERATOR FAMILY .. ADD / DROP
BEGIN TRANSACTION;
CREATE ROLE regress_alter_generic_user6;
CREATE SCHEMA alt_nsp6;
REVOKE ALL ON SCHEMA alt_nsp6 FROM regress_alter_generic_user6;
CREATE OPERATOR FAMILY alt_nsp6.alt_opf6 USING btree;
SET ROLE regress_alter_generic_user6;
ALTER OPERATOR FAMILY alt_nsp6.alt_opf6 USING btree ADD OPERATOR 1 < (int4, int2);
ROLLBACK;

-- Should fail. Only two arguments required for ALTER OPERATOR FAMILY ... DROP OPERATOR
CREATE OPERATOR FAMILY alt_opf7 USING btree;
ALTER OPERATOR FAMILY alt_opf7 USING btree ADD OPERATOR 1 < (int4, int2);
ALTER OPERATOR FAMILY alt_opf7 USING btree DROP OPERATOR 1 (int4, int2, int8);
DROP OPERATOR FAMILY alt_opf7 USING btree;

-- Should work. During ALTER OPERATOR FAMILY ... DROP OPERATOR
-- when left type is the same as right type, a DROP with only one argument type should work
CREATE OPERATOR FAMILY alt_opf8 USING btree;
ALTER OPERATOR FAMILY alt_opf8 USING btree ADD OPERATOR 1 < (int4, int4);
DROP OPERATOR FAMILY alt_opf8 USING btree;

-- Should work. Textbook case of ALTER OPERATOR FAMILY ... ADD OPERATOR with FOR ORDER BY
CREATE OPERATOR FAMILY alt_opf9 USING gist;
ALTER OPERATOR FAMILY alt_opf9 USING gist ADD OPERATOR 1 < (int4, int4) FOR ORDER BY float_ops;
DROP OPERATOR FAMILY alt_opf9 USING gist;

-- Should fail. Ensure correct ordering methods in ALTER OPERATOR FAMILY ... ADD OPERATOR .. FOR ORDER BY
CREATE OPERATOR FAMILY alt_opf10 USING btree;
ALTER OPERATOR FAMILY alt_opf10 USING btree ADD OPERATOR 1 < (int4, int4) FOR ORDER BY float_ops;
DROP OPERATOR FAMILY alt_opf10 USING btree;

-- Should work. Textbook case of ALTER OPERATOR FAMILY ... ADD OPERATOR with FOR ORDER BY
CREATE OPERATOR FAMILY alt_opf11 USING gist;
ALTER OPERATOR FAMILY alt_opf11 USING gist ADD OPERATOR 1 < (int4, int4) FOR ORDER BY float_ops;
ALTER OPERATOR FAMILY alt_opf11 USING gist DROP OPERATOR 1 (int4, int4);
DROP OPERATOR FAMILY alt_opf11 USING gist;

-- Should fail. btree comparison functions should return INTEGER in ALTER OPERATOR FAMILY ... ADD FUNCTION
BEGIN TRANSACTION;
CREATE OPERATOR FAMILY alt_opf12 USING btree;
CREATE FUNCTION fn_opf12  (int4, int2) RETURNS BIGINT AS 'SELECT NULL::BIGINT;' LANGUAGE SQL;
ALTER OPERATOR FAMILY alt_opf12 USING btree ADD FUNCTION 1 fn_opf12(int4, int2);
DROP OPERATOR FAMILY alt_opf12 USING btree;
ROLLBACK;

-- Should fail. hash comparison functions should return INTEGER in ALTER OPERATOR FAMILY ... ADD FUNCTION
BEGIN TRANSACTION;
CREATE OPERATOR FAMILY alt_opf13 USING hash;
CREATE FUNCTION fn_opf13  (int4) RETURNS BIGINT AS 'SELECT NULL::BIGINT;' LANGUAGE SQL;
ALTER OPERATOR FAMILY alt_opf13 USING hash ADD FUNCTION 1 fn_opf13(int4);
DROP OPERATOR FAMILY alt_opf13 USING hash;
ROLLBACK;

-- Should fail. btree comparison functions should have two arguments in ALTER OPERATOR FAMILY ... ADD FUNCTION
BEGIN TRANSACTION;
CREATE OPERATOR FAMILY alt_opf14 USING btree;
CREATE FUNCTION fn_opf14 (int4) RETURNS BIGINT AS 'SELECT NULL::BIGINT;' LANGUAGE SQL;
ALTER OPERATOR FAMILY alt_opf14 USING btree ADD FUNCTION 1 fn_opf14(int4);
DROP OPERATOR FAMILY alt_opf14 USING btree;
ROLLBACK;

-- Should fail. hash comparison functions should have one argument in ALTER OPERATOR FAMILY ... ADD FUNCTION
BEGIN TRANSACTION;
CREATE OPERATOR FAMILY alt_opf15 USING hash;
CREATE FUNCTION fn_opf15 (int4, int2) RETURNS BIGINT AS 'SELECT NULL::BIGINT;' LANGUAGE SQL;
ALTER OPERATOR FAMILY alt_opf15 USING hash ADD FUNCTION 1 fn_opf15(int4, int2);
DROP OPERATOR FAMILY alt_opf15 USING hash;
ROLLBACK;

-- Should fail. In gist throw an error when giving different data types for function argument
-- without defining left / right type in ALTER OPERATOR FAMILY ... ADD FUNCTION
CREATE OPERATOR FAMILY alt_opf16 USING gist;
ALTER OPERATOR FAMILY alt_opf16 USING gist ADD FUNCTION 1 btint42cmp(int4, int2);
DROP OPERATOR FAMILY alt_opf16 USING gist;

-- Should fail. duplicate operator number / function number in ALTER OPERATOR FAMILY ... ADD FUNCTION
CREATE OPERATOR FAMILY alt_opf17 USING btree;
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD OPERATOR 1 < (int4, int4), OPERATOR 1 < (int4, int4); -- operator # appears twice in same statement
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD OPERATOR 1 < (int4, int4); -- operator 1 requested first-time
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD OPERATOR 1 < (int4, int4); -- operator 1 requested again in separate statement
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD
  OPERATOR 1 < (int4, int2) ,
  OPERATOR 2 <= (int4, int2) ,
  OPERATOR 3 = (int4, int2) ,
  OPERATOR 4 >= (int4, int2) ,
  OPERATOR 5 > (int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2);    -- procedure 1 appears twice in same statement
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD
  OPERATOR 1 < (int4, int2) ,
  OPERATOR 2 <= (int4, int2) ,
  OPERATOR 3 = (int4, int2) ,
  OPERATOR 4 >= (int4, int2) ,
  OPERATOR 5 > (int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2);    -- procedure 1 appears first time
ALTER OPERATOR FAMILY alt_opf17 USING btree ADD
  OPERATOR 1 < (int4, int2) ,
  OPERATOR 2 <= (int4, int2) ,
  OPERATOR 3 = (int4, int2) ,
  OPERATOR 4 >= (int4, int2) ,
  OPERATOR 5 > (int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2);    -- procedure 1 requested again in separate statement
DROP OPERATOR FAMILY alt_opf17 USING btree;


-- Should fail. Ensure that DROP requests for missing OPERATOR / FUNCTIONS
-- return appropriate message in ALTER OPERATOR FAMILY ... DROP OPERATOR / FUNCTION
CREATE OPERATOR FAMILY alt_opf18 USING btree;
ALTER OPERATOR FAMILY alt_opf18 USING btree DROP OPERATOR 1 (int4, int4);
ALTER OPERATOR FAMILY alt_opf18 USING btree ADD
  OPERATOR 1 < (int4, int2) ,
  OPERATOR 2 <= (int4, int2) ,
  OPERATOR 3 = (int4, int2) ,
  OPERATOR 4 >= (int4, int2) ,
  OPERATOR 5 > (int4, int2) ,
  FUNCTION 1 btint42cmp(int4, int2);
-- Should fail. Not allowed to have cross-type equalimage function.
ALTER OPERATOR FAMILY alt_opf18 USING btree
  ADD FUNCTION 4 (int4, int2) btequalimage(oid);
ALTER OPERATOR FAMILY alt_opf18 USING btree DROP FUNCTION 2 (int4, int4);
DROP OPERATOR FAMILY alt_opf18 USING btree;

-- Should fail. Invalid opclass options function (#5) specifications.
CREATE OPERATOR FAMILY alt_opf19 USING btree;
ALTER OPERATOR FAMILY alt_opf19 USING btree ADD FUNCTION 5 test_opclass_options_func(internal, text[], bool);
ALTER OPERATOR FAMILY alt_opf19 USING btree ADD FUNCTION 5 (int4) btint42cmp(int4, int2);
ALTER OPERATOR FAMILY alt_opf19 USING btree ADD FUNCTION 5 (int4, int2) btint42cmp(int4, int2);
ALTER OPERATOR FAMILY alt_opf19 USING btree ADD FUNCTION 5 (int4) test_opclass_options_func(internal); -- Ok
ALTER OPERATOR FAMILY alt_opf19 USING btree DROP FUNCTION 5 (int4, int4);
DROP OPERATOR FAMILY alt_opf19 USING btree;

--
-- Statistics
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;
CREATE TABLE alt_regress_1 (a INTEGER, b INTEGER);
CREATE STATISTICS alt_stat1 ON a, b FROM alt_regress_1;
CREATE STATISTICS alt_stat2 ON a, b FROM alt_regress_1;

ALTER STATISTICS alt_stat1 RENAME TO alt_stat2;   -- failed (name conflict)
ALTER STATISTICS alt_stat1 RENAME TO alt_stat3;   -- OK
ALTER STATISTICS alt_stat2 OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER STATISTICS alt_stat2 OWNER TO regress_alter_generic_user3;  -- OK
ALTER STATISTICS alt_stat2 SET SCHEMA alt_nsp2;    -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;
CREATE TABLE alt_regress_2 (a INTEGER, b INTEGER);
CREATE STATISTICS alt_stat1 ON a, b FROM alt_regress_2;
CREATE STATISTICS alt_stat2 ON a, b FROM alt_regress_2;

ALTER STATISTICS alt_stat3 RENAME TO alt_stat4;    -- failed (not owner)
ALTER STATISTICS alt_stat1 RENAME TO alt_stat4;    -- OK
ALTER STATISTICS alt_stat3 OWNER TO regress_alter_generic_user2; -- failed (not owner)
ALTER STATISTICS alt_stat2 OWNER TO regress_alter_generic_user3; -- failed (no role membership)
ALTER STATISTICS alt_stat3 SET SCHEMA alt_nsp2;		-- failed (not owner)
ALTER STATISTICS alt_stat2 SET SCHEMA alt_nsp2;		-- failed (name conflict)

RESET SESSION AUTHORIZATION;
SELECT nspname, stxname, rolname
  FROM pg_statistic_ext s, pg_namespace n, pg_authid a
 WHERE s.stxnamespace = n.oid AND s.stxowner = a.oid
   AND n.nspname in ('alt_nsp1', 'alt_nsp2')
 ORDER BY nspname, stxname;

--
-- Text Search Dictionary
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;
CREATE TEXT SEARCH DICTIONARY alt_ts_dict1 (template=simple);
CREATE TEXT SEARCH DICTIONARY alt_ts_dict2 (template=simple);

ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict2;  -- failed (name conflict)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict3;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regress_alter_generic_user3;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;
CREATE TEXT SEARCH DICTIONARY alt_ts_dict1 (template=simple);
CREATE TEXT SEARCH DICTIONARY alt_ts_dict2 (template=simple);

ALTER TEXT SEARCH DICTIONARY alt_ts_dict3 RENAME TO alt_ts_dict4;  -- failed (not owner)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict4;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict3 OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict3 SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT nspname, dictname, rolname
  FROM pg_ts_dict t, pg_namespace n, pg_authid a
  WHERE t.dictnamespace = n.oid AND t.dictowner = a.oid
    AND n.nspname in ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, dictname;

--
-- Text Search Configuration
--
SET SESSION AUTHORIZATION regress_alter_generic_user1;
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf1 (copy=english);
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf2 (copy=english);

ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf2;  -- failed (name conflict)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf3;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regress_alter_generic_user2;  -- failed (no role membership)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regress_alter_generic_user3;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regress_alter_generic_user2;
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf1 (copy=english);
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf2 (copy=english);

ALTER TEXT SEARCH CONFIGURATION alt_ts_conf3 RENAME TO alt_ts_conf4;  -- failed (not owner)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf4;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf3 OWNER TO regress_alter_generic_user2;  -- failed (not owner)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regress_alter_generic_user3;  -- failed (no role membership)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf3 SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT nspname, cfgname, rolname
  FROM pg_ts_config t, pg_namespace n, pg_authid a
  WHERE t.cfgnamespace = n.oid AND t.cfgowner = a.oid
    AND n.nspname in ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, cfgname;

--
-- Text Search Template
--
CREATE TEXT SEARCH TEMPLATE alt_ts_temp1 (lexize=dsimple_lexize);
CREATE TEXT SEARCH TEMPLATE alt_ts_temp2 (lexize=dsimple_lexize);

ALTER TEXT SEARCH TEMPLATE alt_ts_temp1 RENAME TO alt_ts_temp2; -- failed (name conflict)
ALTER TEXT SEARCH TEMPLATE alt_ts_temp1 RENAME TO alt_ts_temp3; -- OK
ALTER TEXT SEARCH TEMPLATE alt_ts_temp2 SET SCHEMA alt_nsp2;    -- OK

CREATE TEXT SEARCH TEMPLATE alt_ts_temp2 (lexize=dsimple_lexize);
ALTER TEXT SEARCH TEMPLATE alt_ts_temp2 SET SCHEMA alt_nsp2;    -- failed (name conflict)

-- invalid: non-lowercase quoted identifiers
CREATE TEXT SEARCH TEMPLATE tstemp_case ("Init" = init_function);

SELECT nspname, tmplname
  FROM pg_ts_template t, pg_namespace n
  WHERE t.tmplnamespace = n.oid AND nspname like 'alt_nsp%'
  ORDER BY nspname, tmplname;

--
-- Text Search Parser
--

CREATE TEXT SEARCH PARSER alt_ts_prs1
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
CREATE TEXT SEARCH PARSER alt_ts_prs2
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);

ALTER TEXT SEARCH PARSER alt_ts_prs1 RENAME TO alt_ts_prs2; -- failed (name conflict)
ALTER TEXT SEARCH PARSER alt_ts_prs1 RENAME TO alt_ts_prs3; -- OK
ALTER TEXT SEARCH PARSER alt_ts_prs2 SET SCHEMA alt_nsp2;   -- OK

CREATE TEXT SEARCH PARSER alt_ts_prs2
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
ALTER TEXT SEARCH PARSER alt_ts_prs2 SET SCHEMA alt_nsp2;   -- failed (name conflict)

-- invalid: non-lowercase quoted identifiers
CREATE TEXT SEARCH PARSER tspars_case ("Start" = start_function);

SELECT nspname, prsname
  FROM pg_ts_parser t, pg_namespace n
  WHERE t.prsnamespace = n.oid AND nspname like 'alt_nsp%'
  ORDER BY nspname, prsname;

---
--- Cleanup resources
---
DROP FOREIGN DATA WRAPPER alt_fdw2 CASCADE;
DROP FOREIGN DATA WRAPPER alt_fdw3 CASCADE;

DROP LANGUAGE alt_lang2 CASCADE;
DROP LANGUAGE alt_lang3 CASCADE;

DROP SCHEMA alt_nsp1 CASCADE;
DROP SCHEMA alt_nsp2 CASCADE;

DROP USER regress_alter_generic_user1;
DROP USER regress_alter_generic_user2;
DROP USER regress_alter_generic_user3;
