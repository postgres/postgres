--
-- Test for pg_get_object_address
--

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS regtest_addr_user;

CREATE USER regtest_addr_user;

-- Test generic object addressing/identification functions
CREATE SCHEMA addr_nsp;
SET search_path TO 'addr_nsp';
CREATE FOREIGN DATA WRAPPER addr_fdw;
CREATE SERVER addr_fserv FOREIGN DATA WRAPPER addr_fdw;
CREATE TEXT SEARCH DICTIONARY addr_ts_dict (template=simple);
CREATE TEXT SEARCH CONFIGURATION addr_ts_conf (copy=english);
CREATE TEXT SEARCH TEMPLATE addr_ts_temp (lexize=dsimple_lexize);
CREATE TEXT SEARCH PARSER addr_ts_prs
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
CREATE TABLE addr_nsp.gentable (
	a serial primary key CONSTRAINT a_chk CHECK (a > 0),
	b text DEFAULT 'hello');
CREATE VIEW addr_nsp.genview AS SELECT * from addr_nsp.gentable;
CREATE MATERIALIZED VIEW addr_nsp.genmatview AS SELECT * FROM addr_nsp.gentable;
CREATE TYPE addr_nsp.gencomptype AS (a int);
CREATE TYPE addr_nsp.genenum AS ENUM ('one', 'two');
CREATE FOREIGN TABLE addr_nsp.genftable (a int) SERVER addr_fserv;
CREATE AGGREGATE addr_nsp.genaggr(int4) (sfunc = int4pl, stype = int4);
CREATE DOMAIN addr_nsp.gendomain AS int4 CONSTRAINT domconstr CHECK (value > 0);
CREATE FUNCTION addr_nsp.trig() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN END; $$;
CREATE TRIGGER t BEFORE INSERT ON addr_nsp.gentable FOR EACH ROW EXECUTE PROCEDURE addr_nsp.trig();
CREATE POLICY genpol ON addr_nsp.gentable;
CREATE SERVER "integer" FOREIGN DATA WRAPPER addr_fdw;
CREATE USER MAPPING FOR regtest_addr_user SERVER "integer";
ALTER DEFAULT PRIVILEGES FOR ROLE regtest_addr_user IN SCHEMA public GRANT ALL ON TABLES TO regtest_addr_user;
ALTER DEFAULT PRIVILEGES FOR ROLE regtest_addr_user REVOKE DELETE ON TABLES FROM regtest_addr_user;
CREATE TRANSFORM FOR int LANGUAGE SQL (
	FROM SQL WITH FUNCTION varchar_transform(internal),
	TO SQL WITH FUNCTION int4recv(internal));

-- test some error cases
SELECT pg_get_object_address('stone', '{}', '{}');
SELECT pg_get_object_address('table', '{}', '{}');
SELECT pg_get_object_address('table', '{NULL}', '{}');

-- unrecognized object types
DO $$
DECLARE
	objtype text;
BEGIN
	FOR objtype IN VALUES ('toast table'), ('index column'), ('sequence column'),
		('toast table column'), ('view column'), ('materialized view column')
	LOOP
		BEGIN
			PERFORM pg_get_object_address(objtype, '{one}', '{}');
		EXCEPTION WHEN invalid_parameter_value THEN
			RAISE WARNING 'error for %: %', objtype, sqlerrm;
		END;
	END LOOP;
END;
$$;

DO $$
DECLARE
	objtype text;
	names	text[];
	args	text[];
BEGIN
	FOR objtype IN VALUES
		('table'), ('index'), ('sequence'), ('view'),
		('materialized view'), ('foreign table'),
		('table column'), ('foreign table column'),
		('aggregate'), ('function'), ('type'), ('cast'),
		('table constraint'), ('domain constraint'), ('conversion'), ('default value'),
		('operator'), ('operator class'), ('operator family'), ('rule'), ('trigger'),
		('text search parser'), ('text search dictionary'),
		('text search template'), ('text search configuration'),
		('policy'), ('user mapping'), ('default acl'), ('transform'),
		('operator of access method'), ('function of access method')
	LOOP
		FOR names IN VALUES ('{eins}'), ('{addr_nsp, zwei}'), ('{eins, zwei, drei}')
		LOOP
			FOR args IN VALUES ('{}'), ('{integer}')
			LOOP
				BEGIN
					PERFORM pg_get_object_address(objtype, names, args);
				EXCEPTION WHEN OTHERS THEN
						RAISE WARNING 'error for %,%,%: %', objtype, names, args, sqlerrm;
				END;
			END LOOP;
		END LOOP;
	END LOOP;
END;
$$;

-- these object types cannot be qualified names
SELECT pg_get_object_address('language', '{one}', '{}');
SELECT pg_get_object_address('language', '{one,two}', '{}');
SELECT pg_get_object_address('large object', '{123}', '{}');
SELECT pg_get_object_address('large object', '{123,456}', '{}');
SELECT pg_get_object_address('large object', '{blargh}', '{}');
SELECT pg_get_object_address('schema', '{one}', '{}');
SELECT pg_get_object_address('schema', '{one,two}', '{}');
SELECT pg_get_object_address('role', '{one}', '{}');
SELECT pg_get_object_address('role', '{one,two}', '{}');
SELECT pg_get_object_address('database', '{one}', '{}');
SELECT pg_get_object_address('database', '{one,two}', '{}');
SELECT pg_get_object_address('tablespace', '{one}', '{}');
SELECT pg_get_object_address('tablespace', '{one,two}', '{}');
SELECT pg_get_object_address('foreign-data wrapper', '{one}', '{}');
SELECT pg_get_object_address('foreign-data wrapper', '{one,two}', '{}');
SELECT pg_get_object_address('server', '{one}', '{}');
SELECT pg_get_object_address('server', '{one,two}', '{}');
SELECT pg_get_object_address('extension', '{one}', '{}');
SELECT pg_get_object_address('extension', '{one,two}', '{}');
SELECT pg_get_object_address('event trigger', '{one}', '{}');
SELECT pg_get_object_address('event trigger', '{one,two}', '{}');
SELECT pg_get_object_address('access method', '{one}', '{}');
SELECT pg_get_object_address('access method', '{one,two}', '{}');

-- test successful cases
WITH objects (type, name, args) AS (VALUES
				('table', '{addr_nsp, gentable}'::text[], '{}'::text[]),
				('index', '{addr_nsp, gentable_pkey}', '{}'),
				('sequence', '{addr_nsp, gentable_a_seq}', '{}'),
				-- toast table
				('view', '{addr_nsp, genview}', '{}'),
				('materialized view', '{addr_nsp, genmatview}', '{}'),
				('foreign table', '{addr_nsp, genftable}', '{}'),
				('table column', '{addr_nsp, gentable, b}', '{}'),
				('foreign table column', '{addr_nsp, genftable, a}', '{}'),
				('aggregate', '{addr_nsp, genaggr}', '{int4}'),
				('function', '{pg_catalog, pg_identify_object}', '{pg_catalog.oid, pg_catalog.oid, int4}'),
				('type', '{pg_catalog._int4}', '{}'),
				('type', '{addr_nsp.gendomain}', '{}'),
				('type', '{addr_nsp.gencomptype}', '{}'),
				('type', '{addr_nsp.genenum}', '{}'),
				('cast', '{int8}', '{int4}'),
				('collation', '{default}', '{}'),
				('table constraint', '{addr_nsp, gentable, a_chk}', '{}'),
				('domain constraint', '{addr_nsp.gendomain}', '{domconstr}'),
				('conversion', '{pg_catalog, ascii_to_mic}', '{}'),
				('default value', '{addr_nsp, gentable, b}', '{}'),
				('language', '{plpgsql}', '{}'),
				-- large object
				('operator', '{+}', '{int4, int4}'),
				('operator class', '{btree, int4_ops}', '{}'),
				('operator family', '{btree, integer_ops}', '{}'),
				('operator of access method', '{btree,integer_ops,1}', '{integer,integer}'),
				('function of access method', '{btree,integer_ops,2}', '{integer,integer}'),
				('rule', '{addr_nsp, genview, _RETURN}', '{}'),
				('trigger', '{addr_nsp, gentable, t}', '{}'),
				('schema', '{addr_nsp}', '{}'),
				('text search parser', '{addr_ts_prs}', '{}'),
				('text search dictionary', '{addr_ts_dict}', '{}'),
				('text search template', '{addr_ts_temp}', '{}'),
				('text search configuration', '{addr_ts_conf}', '{}'),
				('role', '{regtest_addr_user}', '{}'),
				-- database
				-- tablespace
				('foreign-data wrapper', '{addr_fdw}', '{}'),
				('server', '{addr_fserv}', '{}'),
				('user mapping', '{regtest_addr_user}', '{integer}'),
				('default acl', '{regtest_addr_user,public}', '{r}'),
				('default acl', '{regtest_addr_user}', '{r}'),
				-- extension
				-- event trigger
				('policy', '{addr_nsp, gentable, genpol}', '{}'),
				('transform', '{int}', '{sql}'),
				('access method', '{btree}', '{}')
        )
SELECT (pg_identify_object(addr1.classid, addr1.objid, addr1.subobjid)).*,
	-- test roundtrip through pg_identify_object_as_address
	ROW(pg_identify_object(addr1.classid, addr1.objid, addr1.subobjid)) =
	ROW(pg_identify_object(addr2.classid, addr2.objid, addr2.subobjid))
	  FROM objects, pg_get_object_address(type, name, args) addr1,
			pg_identify_object_as_address(classid, objid, subobjid) ioa(typ,nms,args),
			pg_get_object_address(typ, nms, ioa.args) as addr2
	ORDER BY addr1.classid, addr1.objid, addr1.subobjid;

---
--- Cleanup resources
---
DROP FOREIGN DATA WRAPPER addr_fdw CASCADE;

DROP SCHEMA addr_nsp CASCADE;

DROP OWNED BY regtest_addr_user;
DROP USER regtest_addr_user;
