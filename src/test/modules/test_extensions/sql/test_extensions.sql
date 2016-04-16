-- test some errors
CREATE EXTENSION test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext;
CREATE SCHEMA test_ext;
CREATE EXTENSION test_ext1 SCHEMA test_ext;

-- finally success
CREATE EXTENSION test_ext1 SCHEMA test_ext CASCADE;

SELECT extname, nspname, extversion, extrelocatable FROM pg_extension e, pg_namespace n WHERE extname LIKE 'test_ext%' AND e.extnamespace = n.oid ORDER BY 1;

CREATE EXTENSION test_ext_cyclic1 CASCADE;

DROP SCHEMA test_ext CASCADE;

CREATE EXTENSION test_ext6;
DROP EXTENSION test_ext6;
CREATE EXTENSION test_ext6;
