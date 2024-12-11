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
) WITH (autovacuum_enabled = false);

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

-- test MVCC behavior: changes do not persist after abort (in contrast
-- to pg_restore_relation_stats(), which uses in-place updates).
BEGIN;
SELECT
    pg_catalog.pg_set_relation_stats(
        relation => 'stats_import.test'::regclass,
        relpages => NULL::integer,
        reltuples => 4000.0::real,
        relallvisible => 4::integer);
ABORT;

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

BEGIN;
SELECT
    pg_catalog.pg_clear_relation_stats(
        'stats_import.test'::regclass);
ABORT;

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
  FOR VALUES FROM (0) TO (10)
  WITH (autovacuum_enabled = false);

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

-- error: object doesn't exist
SELECT pg_catalog.pg_set_attribute_stats(
    relation => '0'::oid,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- error: object doesn't exist
SELECT pg_catalog.pg_clear_attribute_stats(
    relation => '0'::oid,
    attname => 'id'::name,
    inherited => false::boolean);

-- error: relation null
SELECT pg_catalog.pg_set_attribute_stats(
    relation => NULL::oid,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- error: attribute is system column
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'xmin'::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- error: attname doesn't exist
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'nope'::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- error: attribute is system column
SELECT pg_catalog.pg_clear_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'ctid'::name,
    inherited => false::boolean);

-- error: attname doesn't exist
SELECT pg_catalog.pg_clear_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'nope'::name,
    inherited => false::boolean);

-- error: attname null
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => NULL::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- error: inherited null
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => NULL::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

-- ok: no stakinds
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.1::real,
    avg_width => 2::integer,
    n_distinct => 0.3::real);

SELECT stanullfrac, stawidth, stadistinct
FROM pg_statistic
WHERE starelid = 'stats_import.test'::regclass;

-- error: mcv / mcf null mismatch
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_freqs => '{0.1,0.2,0.3}'::real[]
    );

-- error: mcv / mcf null mismatch part 2
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_vals => '{1,2,3}'::text
    );

-- error: mcv / mcf type mismatch
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_vals => '{2023-09-30,2024-10-31,3}'::text,
    most_common_freqs => '{0.2,0.1}'::real[]
    );

-- error: mcv cast failure
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_vals => '{2,four,3}'::text,
    most_common_freqs => '{0.3,0.25,0.05}'::real[]
    );

-- ok: mcv+mcf
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_vals => '{2,1,3}'::text,
    most_common_freqs => '{0.3,0.25,0.05}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- error: histogram elements null value
-- this generates no warnings, but perhaps it should
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    histogram_bounds => '{1,NULL,3,4}'::text
    );

-- ok: histogram_bounds
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    histogram_bounds => '{1,2,3,4}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- ok: correlation
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    correlation => 0.5::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- error: scalars can't have mcelem
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_elems => '{1,3}'::text,
    most_common_elem_freqs => '{0.3,0.2,0.2,0.3,0.0}'::real[]
    );

-- error: mcelem / mcelem mismatch
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'tags'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_elems => '{one,two}'::text
    );

-- error: mcelem / mcelem null mismatch part 2
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'tags'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_elem_freqs => '{0.3,0.2,0.2,0.3}'::real[]
    );

-- ok: mcelem
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'tags'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_elems => '{one,three}'::text,
    most_common_elem_freqs => '{0.3,0.2,0.2,0.3,0.0}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- error: scalars can't have elem_count_histogram
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    elem_count_histogram => '{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );
-- error: elem_count_histogram null element
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'tags'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    elem_count_histogram => '{1,1,NULL,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );
-- ok: elem_count_histogram
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'tags'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    elem_count_histogram => '{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- error: scalars can't have range stats
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_empty_frac => 0.5::real,
    range_length_histogram => '{399,499,Infinity}'::text
    );
-- error: range_empty_frac range_length_hist null mismatch
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'arange'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_length_histogram => '{399,499,Infinity}'::text
    );
-- error: range_empty_frac range_length_hist null mismatch part 2
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'arange'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_empty_frac => 0.5::real
    );
-- ok: range_empty_frac + range_length_hist
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'arange'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_empty_frac => 0.5::real,
    range_length_histogram => '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- error: scalars can't have range stats
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'id'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_bounds_histogram => '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );
-- ok: range_bounds_histogram
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'arange'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    range_bounds_histogram => '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- error: cannot set most_common_elems for range type
SELECT pg_catalog.pg_set_attribute_stats(
    relation => 'stats_import.test'::regclass,
    attname => 'arange'::name,
    inherited => false::boolean,
    null_frac => 0.5::real,
    avg_width => 2::integer,
    n_distinct => -0.1::real,
    most_common_vals => '{"[2,3)","[1,2)","[3,4)"}'::text,
    most_common_freqs => '{0.3,0.25,0.05}'::real[],
    histogram_bounds => '{"[1,2)","[2,3)","[3,4)","[4,5)"}'::text,
    correlation => 1.1::real,
    most_common_elems => '{3,1}'::text,
    most_common_elem_freqs => '{0.3,0.2,0.2,0.3,0.0}'::real[],
    range_empty_frac => -0.5::real,
    range_length_histogram => '{399,499,Infinity}'::text,
    range_bounds_histogram => '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );

--
-- Clear attribute stats to try again with restore functions
-- (relation stats were already cleared).
--
SELECT
  pg_catalog.pg_clear_attribute_stats(
        'stats_import.test'::regclass,
        s.attname,
        s.inherited)
FROM pg_catalog.pg_stats AS s
WHERE s.schemaname = 'stats_import'
AND s.tablename = 'test'
ORDER BY s.attname, s.inherited;

-- reject: argument name is NULL
SELECT pg_restore_relation_stats(
        'relation', '0'::oid::regclass,
        'version', 150000::integer,
        NULL, '17'::integer,
        'reltuples', 400::real,
        'relallvisible', 4::integer);

-- reject: argument name is an integer
SELECT pg_restore_relation_stats(
        'relation', '0'::oid::regclass,
        'version', 150000::integer,
        17, '17'::integer,
        'reltuples', 400::real,
        'relallvisible', 4::integer);

-- reject: odd number of variadic arguments cannot be pairs
SELECT pg_restore_relation_stats(
        'relation', '0'::oid::regclass,
        'version', 150000::integer,
        'relpages', '17'::integer,
        'reltuples', 400::real,
        'relallvisible');

-- reject: object doesn't exist
SELECT pg_restore_relation_stats(
        'relation', '0'::oid::regclass,
        'version', 150000::integer,
        'relpages', '17'::integer,
        'reltuples', 400::real,
        'relallvisible', 4::integer);

-- ok: set all stats
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'relpages', '17'::integer,
        'reltuples', 400::real,
        'relallvisible', 4::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: just relpages
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'relpages', '15'::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- test non-MVCC behavior: new value should persist after abort
BEGIN;
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'relpages', '16'::integer);
ABORT;

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: just reltuples
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'reltuples', '500'::real);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: just relallvisible
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'relallvisible', 5::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- warn and error: unrecognized argument name
SELECT pg_restore_relation_stats(
        'relation', '0'::oid::regclass,
        'version', 150000::integer,
        'relpages', '17'::integer,
        'reltuples', 400::real,
        'nope', 4::integer);

-- warn: bad relpages type
SELECT pg_restore_relation_stats(
        'relation', 'stats_import.test'::regclass,
        'version', 150000::integer,
        'relpages', 'nope'::text,
        'reltuples', 400.0::real,
        'relallvisible', 4::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- error: object does not exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', '0'::oid::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- error: relation null
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', NULL::oid,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- error: attname null
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', NULL::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- error: attname doesn't exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'nope'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- error: inherited null
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', NULL::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- ok: no stakinds
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.4::real,
    'avg_width', 5::integer,
    'n_distinct', 0.6::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: unrecognized argument name
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.2::real,
    'avg_width', NULL::integer,
    'nope', 0.5::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv / mcf null mismatch part 1
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.6::real,
    'avg_width', 7::integer,
    'n_distinct', -0.7::real,
    'most_common_freqs', '{0.1,0.2,0.3}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv / mcf null mismatch part 2
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.7::real,
    'avg_width', 8::integer,
    'n_distinct', -0.8::real,
    'most_common_vals', '{1,2,3}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv / mcf type mismatch
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.8::real,
    'avg_width', 9::integer,
    'n_distinct', -0.9::real,
    'most_common_vals', '{2,1,3}'::text,
    'most_common_freqs', '{0.2,0.1}'::double precision[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv cast failure
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.9::real,
    'avg_width', 10::integer,
    'n_distinct', -0.4::real,
    'most_common_vals', '{2,four,3}'::text,
    'most_common_freqs', '{0.3,0.25,0.05}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- ok: mcv+mcf
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 1::integer,
    'n_distinct', -0.1::real,
    'most_common_vals', '{2,1,3}'::text,
    'most_common_freqs', '{0.3,0.25,0.05}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: NULL in histogram array
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.2::real,
    'avg_width', 2::integer,
    'n_distinct', -0.2::real,
    'histogram_bounds', '{1,NULL,3,4}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- ok: histogram_bounds
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.3::real,
    'avg_width', 3::integer,
    'n_distinct', -0.3::real,
    'histogram_bounds', '{1,2,3,4}'::text );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: elem_count_histogram null element
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'tags'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.4::real,
    'avg_width', 5::integer,
    'n_distinct', -0.4::real,
    'elem_count_histogram', '{1,1,NULL,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- ok: elem_count_histogram
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'tags'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.5::real,
    'avg_width', 6::integer,
    'n_distinct', -0.55::real,
    'elem_count_histogram', '{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- range stats on a scalar type
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.6::real,
    'avg_width', 7::integer,
    'n_distinct', -0.15::real,
    'range_empty_frac', 0.5::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: range_empty_frac range_length_hist null mismatch
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'arange'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.7::real,
    'avg_width', 8::integer,
    'n_distinct', -0.25::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: range_empty_frac range_length_hist null mismatch part 2
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'arange'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.8::real,
    'avg_width', 9::integer,
    'n_distinct', -0.35::real,
    'range_empty_frac', 0.5::real
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- ok: range_empty_frac + range_length_hist
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'arange'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.9::real,
    'avg_width', 1::integer,
    'n_distinct', -0.19::real,
    'range_empty_frac', 0.5::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: range bounds histogram on scalar
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'id'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', -0.29::real,
    'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- ok: range_bounds_histogram
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'arange'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.2::real,
    'avg_width', 3::integer,
    'n_distinct', -0.39::real,
    'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: too many stat kinds
SELECT pg_catalog.pg_restore_attribute_stats(
    'relation', 'stats_import.test'::regclass,
    'attname', 'arange'::name,
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.5::real,
    'avg_width', 2::integer,
    'n_distinct', -0.1::real,
    'most_common_vals', '{"[2,3)","[1,3)","[3,9)"}'::text,
    'most_common_freqs', '{0.3,0.25,0.05}'::real[],
    'histogram_bounds', '{"[1,2)","[2,3)","[3,4)","[4,)"}'::text,
    'correlation', 1.1::real,
    'most_common_elems', '{3,1}'::text,
    'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[],
    'range_empty_frac', -0.5::real,
    'range_length_histogram', '{399,499,Infinity}'::text,
    'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text);

--
-- Test the ability to exactly copy data from one table to an identical table,
-- correctly reconstructing the stakind order as well as the staopN and
-- stacollN values. Because oids are not stable across databases, we can only
-- test this when the source and destination are on the same database
-- instance. For that reason, we borrow and adapt a query found in fe_utils
-- and used by pg_dump/pg_upgrade.
--
INSERT INTO stats_import.test
SELECT 1, 'one', (1, 1.1, 'ONE', '2001-01-01', '{ "xkey": "xval" }')::stats_import.complex_type, int4range(1,4), array['red','green']
UNION ALL
SELECT 2, 'two', (2, 2.2, 'TWO', '2002-02-02', '[true, 4, "six"]')::stats_import.complex_type,  int4range(1,4), array['blue','yellow']
UNION ALL
SELECT 3, 'tre', (3, 3.3, 'TRE', '2003-03-03', NULL)::stats_import.complex_type, int4range(-1,1), array['"orange"', 'purple', 'cyan']
UNION ALL
SELECT 4, 'four', NULL, int4range(0,100), NULL;

CREATE INDEX is_odd ON stats_import.test(((comp).a % 2 = 1));

-- Generate statistics on table with data
ANALYZE stats_import.test;

CREATE TABLE stats_import.test_clone ( LIKE stats_import.test )
    WITH (autovacuum_enabled = false);

CREATE INDEX is_odd_clone ON stats_import.test_clone(((comp).a % 2 = 1));

--
-- Copy stats from test to test_clone, and is_odd to is_odd_clone
--
SELECT s.schemaname, s.tablename, s.attname, s.inherited
FROM pg_catalog.pg_stats AS s
CROSS JOIN LATERAL
    pg_catalog.pg_set_attribute_stats(
        relation => ('stats_import.' || s.tablename || '_clone')::regclass::oid,
        attname => s.attname,
        inherited => s.inherited,
        null_frac => s.null_frac,
        avg_width => s.avg_width,
        n_distinct => s.n_distinct,
        most_common_vals => s.most_common_vals::text,
        most_common_freqs => s.most_common_freqs,
        histogram_bounds => s.histogram_bounds::text,
        correlation => s.correlation,
        most_common_elems => s.most_common_elems::text,
        most_common_elem_freqs => s.most_common_elem_freqs,
        elem_count_histogram => s.elem_count_histogram,
        range_bounds_histogram => s.range_bounds_histogram::text,
        range_empty_frac => s.range_empty_frac,
        range_length_histogram => s.range_length_histogram::text) AS r
WHERE s.schemaname = 'stats_import'
AND s.tablename IN ('test', 'is_odd')
ORDER BY s.tablename, s.attname, s.inherited;

SELECT c.relname, COUNT(*) AS num_stats
FROM pg_class AS c
JOIN pg_statistic s ON s.starelid = c.oid
WHERE c.relnamespace = 'stats_import'::regnamespace
AND c.relname IN ('test', 'test_clone', 'is_odd', 'is_odd_clone')
GROUP BY c.relname
ORDER BY c.relname;

-- check test minus test_clone
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test_clone'::regclass;

-- check test_clone minus test
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test_clone'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test'::regclass;

-- check is_odd minus is_odd_clone
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd_clone'::regclass;

-- check is_odd_clone minus is_odd
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd_clone'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd'::regclass;

--
SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

--
-- Clear clone stats to try again with pg_restore_attribute_stats
--
SELECT
  pg_catalog.pg_clear_attribute_stats(
        ('stats_import.' || s.tablename)::regclass,
        s.attname,
        s.inherited)
FROM pg_catalog.pg_stats AS s
WHERE s.schemaname = 'stats_import'
AND s.tablename IN ('test_clone', 'is_odd_clone')
ORDER BY s.tablename, s.attname, s.inherited;
SELECT

SELECT COUNT(*)
FROM pg_catalog.pg_stats AS s
WHERE s.schemaname = 'stats_import'
AND s.tablename IN ('test_clone', 'is_odd_clone');

--
-- Copy stats from test to test_clone, and is_odd to is_odd_clone
--
SELECT s.schemaname, s.tablename, s.attname, s.inherited, r.*
FROM pg_catalog.pg_stats AS s
CROSS JOIN LATERAL
    pg_catalog.pg_restore_attribute_stats(
        'relation', ('stats_import.' || s.tablename || '_clone')::regclass,
        'attname', s.attname,
        'inherited', s.inherited,
        'version', 150000,
        'null_frac', s.null_frac,
        'avg_width', s.avg_width,
        'n_distinct', s.n_distinct,
        'most_common_vals', s.most_common_vals::text,
        'most_common_freqs', s.most_common_freqs,
        'histogram_bounds', s.histogram_bounds::text,
        'correlation', s.correlation,
        'most_common_elems', s.most_common_elems::text,
        'most_common_elem_freqs', s.most_common_elem_freqs,
        'elem_count_histogram', s.elem_count_histogram,
        'range_bounds_histogram', s.range_bounds_histogram::text,
        'range_empty_frac', s.range_empty_frac,
        'range_length_histogram', s.range_length_histogram::text) AS r
WHERE s.schemaname = 'stats_import'
AND s.tablename IN ('test', 'is_odd')
ORDER BY s.tablename, s.attname, s.inherited;

SELECT c.relname, COUNT(*) AS num_stats
FROM pg_class AS c
JOIN pg_statistic s ON s.starelid = c.oid
WHERE c.relnamespace = 'stats_import'::regnamespace
AND c.relname IN ('test', 'test_clone', 'is_odd', 'is_odd_clone')
GROUP BY c.relname
ORDER BY c.relname;

-- check test minus test_clone
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test_clone'::regclass;

-- check test_clone minus test
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test_clone'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'test_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.test'::regclass;

-- check is_odd minus is_odd_clone
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd_clone'::regclass;

-- check is_odd_clone minus is_odd
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd_clone'::regclass
EXCEPT
SELECT
    a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
    s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
    s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
    s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
    s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
    s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
    s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
    s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
FROM pg_statistic s
JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE s.starelid = 'stats_import.is_odd'::regclass;

DROP SCHEMA stats_import CASCADE;
