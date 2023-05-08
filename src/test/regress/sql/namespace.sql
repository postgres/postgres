--
-- Regression tests for schemas (namespaces)
--

-- set the whitespace-only search_path to test that the
-- GUC list syntax is preserved during a schema creation
SELECT pg_catalog.set_config('search_path', ' ', false);

CREATE SCHEMA test_ns_schema_1
       CREATE UNIQUE INDEX abc_a_idx ON abc (a)

       CREATE VIEW abc_view AS
              SELECT a+1 AS a, b+1 AS b FROM abc

       CREATE TABLE abc (
              a serial,
              b int UNIQUE
       );

-- verify that the correct search_path restored on abort
SET search_path to public;
BEGIN;
SET search_path to public, test_ns_schema_1;
CREATE SCHEMA test_ns_schema_2
       CREATE VIEW abc_view AS SELECT c FROM abc;
COMMIT;
SHOW search_path;

-- verify that the correct search_path preserved
-- after creating the schema and on commit
BEGIN;
SET search_path to public, test_ns_schema_1;
CREATE SCHEMA test_ns_schema_2
       CREATE VIEW abc_view AS SELECT a FROM abc;
SHOW search_path;
COMMIT;
SHOW search_path;
DROP SCHEMA test_ns_schema_2 CASCADE;

-- verify that the objects were created
SELECT COUNT(*) FROM pg_class WHERE relnamespace =
    (SELECT oid FROM pg_namespace WHERE nspname = 'test_ns_schema_1');

INSERT INTO test_ns_schema_1.abc DEFAULT VALUES;
INSERT INTO test_ns_schema_1.abc DEFAULT VALUES;
INSERT INTO test_ns_schema_1.abc DEFAULT VALUES;

SELECT * FROM test_ns_schema_1.abc;
SELECT * FROM test_ns_schema_1.abc_view;

ALTER SCHEMA test_ns_schema_1 RENAME TO test_ns_schema_renamed;
SELECT COUNT(*) FROM pg_class WHERE relnamespace =
    (SELECT oid FROM pg_namespace WHERE nspname = 'test_ns_schema_1');

-- test IF NOT EXISTS cases
CREATE SCHEMA test_ns_schema_renamed; -- fail, already exists
CREATE SCHEMA IF NOT EXISTS test_ns_schema_renamed; -- ok with notice
CREATE SCHEMA IF NOT EXISTS test_ns_schema_renamed -- fail, disallowed
       CREATE TABLE abc (
              a serial,
              b int UNIQUE
       );

DROP SCHEMA test_ns_schema_renamed CASCADE;

-- verify that the objects were dropped
SELECT COUNT(*) FROM pg_class WHERE relnamespace =
    (SELECT oid FROM pg_namespace WHERE nspname = 'test_ns_schema_renamed');
