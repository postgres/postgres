CREATE SCHEMA stats_import;

CREATE TYPE stats_import.complex_type AS (
    a integer,
    b real,
    c text,
    d date,
    e jsonb);

CREATE TABLE stats_import.test(
    id INTEGER PRIMARY KEY,
    name text,
    comp stats_import.complex_type,
    arange int4range,
    tags text[]
);

-- starting stats
SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- error: regclass not found
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 0::Oid,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

-- relpages default
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => NULL::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

-- reltuples default
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => NULL::real,
        relallvisible => 4::integer);

-- relallvisible default
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => NULL::integer);

-- named arguments
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => 17::integer,
        reltuples => 400.0::real,
        relallvisible => 4::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- positional arguments
SELECT
    pg_catalog.pg_set_relation_stats(
        'stats_import.test'::regclass,
        18::integer,
        401.0::real,
        5::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- clear
SELECT
    pg_catalog.pg_clear_relation_stats(
        'stats_import.test'::regclass);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- invalid relkinds for statistics
CREATE SEQUENCE stats_import.testseq;
CREATE VIEW stats_import.testview AS SELECT * FROM stats_import.test;
SELECT
    pg_catalog.pg_clear_relation_stats(
        'stats_import.testseq'::regclass);
SELECT
    pg_catalog.pg_clear_relation_stats(
        'stats_import.testview'::regclass);

--  relpages may be -1 for partitioned tables
CREATE TABLE stats_import.part_parent ( i integer ) PARTITION BY RANGE(i);
CREATE TABLE stats_import.part_child_1
  PARTITION OF stats_import.part_parent
  FOR VALUES FROM (0) TO (10);

ANALYZE stats_import.part_parent;

SELECT relpages
FROM pg_class
WHERE oid = 'stats_import.part_parent'::regclass;

-- although partitioned tables have no storage, setting relpages to a
-- positive value is still allowed
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.part_parent'::regclass,
        relpages => 2::integer);

-- nothing stops us from setting it to -1
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.part_parent'::regclass,
        relpages => -1::integer);

DROP SCHEMA stats_import CASCADE;
