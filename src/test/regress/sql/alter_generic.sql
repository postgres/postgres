--
-- Test for ALTER some_object {RENAME TO, OWNER TO, SET SCHEMA}
--

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS regtest_alter_user1;
DROP ROLE IF EXISTS regtest_alter_user2;
DROP ROLE IF EXISTS regtest_alter_user3;

RESET client_min_messages;

CREATE USER regtest_alter_user3;
CREATE USER regtest_alter_user2;
CREATE USER regtest_alter_user1 IN ROLE regtest_alter_user3;

CREATE SCHEMA alt_nsp1;
CREATE SCHEMA alt_nsp2;

GRANT ALL ON SCHEMA alt_nsp1, alt_nsp2 TO public;

SET search_path = alt_nsp1, public;

--
-- Function and Aggregate
--
SET SESSION AUTHORIZATION regtest_alter_user1;
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
ALTER AGGREGATE alt_func1(int) OWNER TO regtest_alter_user3;  -- failed (not aggregate)
ALTER AGGREGATE alt_func1(int) SET SCHEMA alt_nsp2;  -- failed (not aggregate)

ALTER FUNCTION alt_func1(int) RENAME TO alt_func2;  -- failed (name conflict)
ALTER FUNCTION alt_func1(int) RENAME TO alt_func3;  -- OK
ALTER FUNCTION alt_func2(int) OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER FUNCTION alt_func2(int) OWNER TO regtest_alter_user3;  -- OK
ALTER FUNCTION alt_func2(int) SET SCHEMA alt_nsp2;  -- OK

ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg2;   -- failed (name conflict)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg3;   -- OK
ALTER AGGREGATE alt_agg2(int) OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER AGGREGATE alt_agg2(int) OWNER TO regtest_alter_user3;  -- OK
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regtest_alter_user2;
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
ALTER FUNCTION alt_func3(int) OWNER TO regtest_alter_user2;	-- failed (not owner)
ALTER FUNCTION alt_func2(int) OWNER TO regtest_alter_user3;	-- failed (no role membership)
ALTER FUNCTION alt_func3(int) SET SCHEMA alt_nsp2;      -- failed (not owner)
ALTER FUNCTION alt_func2(int) SET SCHEMA alt_nsp2;	-- failed (name conflicts)

ALTER AGGREGATE alt_agg3(int) RENAME TO alt_agg4;   -- failed (not owner)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg4;   -- OK
ALTER AGGREGATE alt_agg3(int) OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER AGGREGATE alt_agg2(int) OWNER TO regtest_alter_user3;  -- failed (no role membership)
ALTER AGGREGATE alt_agg3(int) SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- failed (name conflict)

RESET SESSION AUTHORIZATION;

SELECT n.nspname, proname, prorettype::regtype, proisagg, a.rolname
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
SET SESSION AUTHORIZATION regtest_alter_user1;
CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv1 RENAME TO alt_conv2;  -- failed (name conflict)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv3;  -- OK
ALTER CONVERSION alt_conv2 OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER CONVERSION alt_conv2 OWNER TO regtest_alter_user3;  -- OK
ALTER CONVERSION alt_conv2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regtest_alter_user2;
CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv3 RENAME TO alt_conv4;  -- failed (not owner)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv4;  -- OK
ALTER CONVERSION alt_conv3 OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER CONVERSION alt_conv2 OWNER TO regtest_alter_user3;  -- failed (no role membership)
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

ALTER LANGUAGE alt_lang1 OWNER TO regtest_alter_user1;  -- OK
ALTER LANGUAGE alt_lang2 OWNER TO regtest_alter_user2;  -- OK

SET SESSION AUTHORIZATION regtest_alter_user1;
ALTER LANGUAGE alt_lang1 RENAME TO alt_lang2;   -- failed (name conflict)
ALTER LANGUAGE alt_lang2 RENAME TO alt_lang3;   -- failed (not owner)
ALTER LANGUAGE alt_lang1 RENAME TO alt_lang3;   -- OK

ALTER LANGUAGE alt_lang2 OWNER TO regtest_alter_user3;  -- failed (not owner)
ALTER LANGUAGE alt_lang3 OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER LANGUAGE alt_lang3 OWNER TO regtest_alter_user3;  -- OK

RESET SESSION AUTHORIZATION;
SELECT lanname, a.rolname
  FROM pg_language l, pg_authid a
  WHERE l.lanowner = a.oid AND l.lanname like 'alt_lang%'
  ORDER BY lanname;

--
-- Operator
--
SET SESSION AUTHORIZATION regtest_alter_user1;

CREATE OPERATOR @-@ ( leftarg = int4, rightarg = int4, procedure = int4mi );
CREATE OPERATOR @+@ ( leftarg = int4, rightarg = int4, procedure = int4pl );

ALTER OPERATOR @+@(int4, int4) OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER OPERATOR @+@(int4, int4) OWNER TO regtest_alter_user3;  -- OK
ALTER OPERATOR @-@(int4, int4) SET SCHEMA alt_nsp2;           -- OK

SET SESSION AUTHORIZATION regtest_alter_user2;

CREATE OPERATOR @-@ ( leftarg = int4, rightarg = int4, procedure = int4mi );

ALTER OPERATOR @+@(int4, int4) OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER OPERATOR @-@(int4, int4) OWNER TO regtest_alter_user3;  -- failed (no role membership)
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
ALTER OPERATOR FAMILY alt_opf1 USING hash OWNER TO regtest_alter_user1;
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regtest_alter_user1;

CREATE OPERATOR CLASS alt_opc1 FOR TYPE uuid USING hash AS STORAGE uuid;
CREATE OPERATOR CLASS alt_opc2 FOR TYPE uuid USING hash AS STORAGE uuid;
ALTER OPERATOR CLASS alt_opc1 USING hash OWNER TO regtest_alter_user1;
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regtest_alter_user1;

SET SESSION AUTHORIZATION regtest_alter_user1;

ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf2;  -- failed (name conflict)
ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf3;  -- OK
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regtest_alter_user3;  -- OK
ALTER OPERATOR FAMILY alt_opf2 USING hash SET SCHEMA alt_nsp2;  -- OK

ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc2;  -- failed (name conflict)
ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc3;  -- OK
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regtest_alter_user3;  -- OK
ALTER OPERATOR CLASS alt_opc2 USING hash SET SCHEMA alt_nsp2;  -- OK

RESET SESSION AUTHORIZATION;

CREATE OPERATOR FAMILY alt_opf1 USING hash;
CREATE OPERATOR FAMILY alt_opf2 USING hash;
ALTER OPERATOR FAMILY alt_opf1 USING hash OWNER TO regtest_alter_user2;
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regtest_alter_user2;

CREATE OPERATOR CLASS alt_opc1 FOR TYPE macaddr USING hash AS STORAGE macaddr;
CREATE OPERATOR CLASS alt_opc2 FOR TYPE macaddr USING hash AS STORAGE macaddr;
ALTER OPERATOR CLASS alt_opc1 USING hash OWNER TO regtest_alter_user2;
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regtest_alter_user2;

SET SESSION AUTHORIZATION regtest_alter_user2;

ALTER OPERATOR FAMILY alt_opf3 USING hash RENAME TO alt_opf4;	-- failed (not owner)
ALTER OPERATOR FAMILY alt_opf1 USING hash RENAME TO alt_opf4;  -- OK
ALTER OPERATOR FAMILY alt_opf3 USING hash OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER OPERATOR FAMILY alt_opf2 USING hash OWNER TO regtest_alter_user3;  -- failed (no role membership)
ALTER OPERATOR FAMILY alt_opf3 USING hash SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER OPERATOR FAMILY alt_opf2 USING hash SET SCHEMA alt_nsp2;  -- failed (name conflict)

ALTER OPERATOR CLASS alt_opc3 USING hash RENAME TO alt_opc4;	-- failed (not owner)
ALTER OPERATOR CLASS alt_opc1 USING hash RENAME TO alt_opc4;  -- OK
ALTER OPERATOR CLASS alt_opc3 USING hash OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER OPERATOR CLASS alt_opc2 USING hash OWNER TO regtest_alter_user3;  -- failed (no role membership)
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

--
-- Text Search Dictionary
--
SET SESSION AUTHORIZATION regtest_alter_user1;
CREATE TEXT SEARCH DICTIONARY alt_ts_dict1 (template=simple);
CREATE TEXT SEARCH DICTIONARY alt_ts_dict2 (template=simple);

ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict2;  -- failed (name conflict)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict3;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regtest_alter_user3;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regtest_alter_user2;
CREATE TEXT SEARCH DICTIONARY alt_ts_dict1 (template=simple);
CREATE TEXT SEARCH DICTIONARY alt_ts_dict2 (template=simple);

ALTER TEXT SEARCH DICTIONARY alt_ts_dict3 RENAME TO alt_ts_dict4;  -- failed (not owner)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict1 RENAME TO alt_ts_dict4;  -- OK
ALTER TEXT SEARCH DICTIONARY alt_ts_dict3 OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER TEXT SEARCH DICTIONARY alt_ts_dict2 OWNER TO regtest_alter_user3;  -- failed (no role membership)
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
SET SESSION AUTHORIZATION regtest_alter_user1;
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf1 (copy=english);
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf2 (copy=english);

ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf2;  -- failed (name conflict)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf3;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regtest_alter_user2;  -- failed (no role membership)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regtest_alter_user3;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 SET SCHEMA alt_nsp2;  -- OK

SET SESSION AUTHORIZATION regtest_alter_user2;
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf1 (copy=english);
CREATE TEXT SEARCH CONFIGURATION alt_ts_conf2 (copy=english);

ALTER TEXT SEARCH CONFIGURATION alt_ts_conf3 RENAME TO alt_ts_conf4;  -- failed (not owner)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf1 RENAME TO alt_ts_conf4;  -- OK
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf3 OWNER TO regtest_alter_user2;  -- failed (not owner)
ALTER TEXT SEARCH CONFIGURATION alt_ts_conf2 OWNER TO regtest_alter_user3;  -- failed (no role membership)
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
DROP LANGUAGE alt_lang4 CASCADE;

DROP SCHEMA alt_nsp1 CASCADE;
DROP SCHEMA alt_nsp2 CASCADE;

DROP USER regtest_alter_user1;
DROP USER regtest_alter_user2;
DROP USER regtest_alter_user3;
