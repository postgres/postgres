--
-- Regression tests for schemas (namespaces)
--

CREATE SCHEMA test_schema_1
       CREATE UNIQUE INDEX abc_a_idx ON abc (a)

       CREATE VIEW abc_view AS
              SELECT a+1 AS a, b+1 AS b FROM abc

       CREATE TABLE abc (
              a serial,
              b int UNIQUE
       );

-- verify that the objects were created
SELECT COUNT(*) FROM pg_class WHERE relnamespace =
    (SELECT oid FROM pg_namespace WHERE nspname = 'test_schema_1');

INSERT INTO test_schema_1.abc DEFAULT VALUES;
INSERT INTO test_schema_1.abc DEFAULT VALUES;
INSERT INTO test_schema_1.abc DEFAULT VALUES;

SELECT * FROM test_schema_1.abc;
SELECT * FROM test_schema_1.abc_view;

DROP SCHEMA test_schema_1 CASCADE;

-- verify that the objects were dropped
SELECT COUNT(*) FROM pg_class WHERE relnamespace =
    (SELECT oid FROM pg_namespace WHERE nspname = 'test_schema_1');
