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

CREATE TABLE stats_import.test_mr(
    id INTEGER PRIMARY KEY,
    name text,
    mrange int4multirange
) WITH (autovacuum_enabled = false);

SELECT
    pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relpages', 18::integer,
        'reltuples', 21::real,
        'relallvisible', 24::integer,
        'relallfrozen', 27::integer);

-- CREATE INDEX on a table with autovac disabled should not overwrite
-- stats
CREATE INDEX test_i ON stats_import.test(id);

SELECT relname, relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass
ORDER BY relname;

SELECT pg_clear_relation_stats('stats_import', 'test');

--
-- relstats tests
--

-- error: schemaname missing
SELECT pg_catalog.pg_restore_relation_stats(
        'relname', 'test',
        'relpages', 17::integer);

-- error: relname missing
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relpages', 17::integer);

--- error: schemaname is wrong type
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 3.6::float,
        'relname', 'test',
        'relpages', 17::integer);

--- error: relname is wrong type
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 0::oid,
        'relpages', 17::integer);

-- error: relation not found
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'nope',
        'relpages', 17::integer);

-- error: odd number of variadic arguments cannot be pairs
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relallvisible');

-- error: argument name is NULL
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        NULL, '17'::integer);

-- starting stats
SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test_i'::regclass;

-- regular indexes have special case locking rules
BEGIN;
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test_i',
        'relpages', 18::integer);

SELECT mode FROM pg_locks
WHERE relation = 'stats_import.test'::regclass AND
      pid = pg_backend_pid() AND granted;

SELECT mode FROM pg_locks
WHERE relation = 'stats_import.test_i'::regclass AND
      pid = pg_backend_pid() AND granted;

COMMIT;

--  relpages may be -1 for partitioned tables
CREATE TABLE stats_import.part_parent ( i integer ) PARTITION BY RANGE(i);
CREATE TABLE stats_import.part_child_1
  PARTITION OF stats_import.part_parent
  FOR VALUES FROM (0) TO (10)
  WITH (autovacuum_enabled = false);

-- This ensures the presence of extended statistics marked with
-- inherited = true.
CREATE STATISTICS stats_import.part_parent_stat
  ON i, (i % 2)
  FROM stats_import.part_parent;

CREATE INDEX part_parent_i ON stats_import.part_parent(i);

INSERT INTO stats_import.part_parent
SELECT g.g
FROM generate_series(0,9) AS g(g);

SELECT COUNT(*) FROM stats_import.part_parent;
SELECT COUNT(*) FROM stats_import.part_child_1;

ANALYZE stats_import.part_parent;

SELECT COUNT(*), e.inherited FROM pg_stats_ext AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'part_parent_stat' GROUP BY e.inherited;

SELECT relpages
FROM pg_class
WHERE oid = 'stats_import.part_parent'::regclass;

--
-- Partitioned indexes aren't analyzed but it is possible to set
-- stats. The locking rules are different from normal indexes due to
-- the rules for in-place updates: both the partitioned table and the
-- partitioned index are locked in ShareUpdateExclusive mode.
--
BEGIN;

SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'part_parent_i',
        'relpages', 2::integer);

SELECT mode FROM pg_locks
WHERE relation = 'stats_import.part_parent'::regclass AND
      pid = pg_backend_pid() AND granted;

SELECT mode FROM pg_locks
WHERE relation = 'stats_import.part_parent_i'::regclass AND
      pid = pg_backend_pid() AND granted;

COMMIT;

SELECT relpages
FROM pg_class
WHERE oid = 'stats_import.part_parent_i'::regclass;

-- ok: set all relstats, with version, no bounds checking
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'version', 150000::integer,
        'relpages', '-17'::integer,
        'reltuples', 400::real,
        'relallvisible', 4::integer,
        'relallfrozen', 2::integer);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: set just relpages, rest stay same
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relpages', '16'::integer);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: set just reltuples, rest stay same
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'reltuples', '500'::real);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: set just relallvisible, rest stay same
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relallvisible', 5::integer);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: just relallfrozen
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'version', 150000::integer,
        'relallfrozen', 3::integer);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- warn: bad relpages type, rest updated
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relpages', 'nope'::text,
        'reltuples', 400.0::real,
        'relallvisible', 4::integer,
        'relallfrozen', 3::integer);

SELECT relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- unrecognized argument name, rest ok
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relpages', '171'::integer,
        'nope', 10::integer);

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- ok: clear stats
SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'test');

SELECT relpages, reltuples, relallvisible
FROM pg_class
WHERE oid = 'stats_import.test'::regclass;

-- invalid relkinds for statistics
CREATE SEQUENCE stats_import.testseq;

SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'testseq');

SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'testseq');

CREATE VIEW stats_import.testview AS SELECT * FROM stats_import.test;

SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'testview');

--
-- attribute stats
--

-- error: schemaname missing
SELECT pg_catalog.pg_restore_attribute_stats(
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: schema does not exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'nope',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname missing
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname does not exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'nope',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname null
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', NULL,
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: NULL attname
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', NULL,
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: attname doesn't exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'nope',
    'inherited', false::boolean,
    'null_frac', 0.1::real,
    'avg_width', 2::integer,
    'n_distinct', 0.3::real);

-- error: both attname and attnum
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'attnum', 1::smallint,
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: neither attname nor attnum
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: attribute is system column
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'xmin',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: inherited null
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', NULL::boolean,
    'null_frac', 0.1::real);

-- ok: just the fixed values, with version, no stakinds
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'version', 150000::integer,
    'null_frac', 0.2::real,
    'avg_width', 5::integer,
    'n_distinct', 0.6::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

--
-- ok: restore by attnum, we normally reserve this for
-- indexes, but there is no reason it shouldn't work
-- for any stat-having relation.
--
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attnum', 1::smallint,
    'inherited', false::boolean,
    'null_frac', 0.4::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: unrecognized argument name, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.2::real,
    'nope', 0.5::real);

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv / mcf null mismatch part 1, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.21::real,
    'most_common_freqs', '{0.1,0.2,0.3}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv / mcf null mismatch part 2, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.21::real,
    'most_common_vals', '{1,2,3}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcf type mismatch, mcv-pair fails, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.22::real,
    'most_common_vals', '{2,1,3}'::text,
    'most_common_freqs', '{0.2,0.1}'::double precision[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcv cast failure, mcv-pair fails, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.23::real,
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
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'most_common_vals', '{2,1,3}'::text,
    'most_common_freqs', '{0.3,0.25,0.05}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: NULL in histogram array, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.24::real,
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
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'histogram_bounds', '{1,2,3,4}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: elem_count_histogram null element, rest get set
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'tags',
    'inherited', false::boolean,
    'null_frac', 0.25::real,
    'elem_count_histogram', '{1,1,NULL,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- ok: elem_count_histogram
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'tags',
    'inherited', false::boolean,
    'null_frac', 0.26::real,
    'elem_count_histogram', '{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- warn: range stats on a scalar type, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.27::real,
    'range_empty_frac', 0.5::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: range_empty_frac range_length_hist null mismatch, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'arange',
    'inherited', false::boolean,
    'null_frac', 0.28::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: range_empty_frac range_length_hist null mismatch part 2, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'arange',
    'inherited', false::boolean,
    'null_frac', 0.29::real,
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
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'arange',
    'inherited', false::boolean,
    'range_empty_frac', 0.5::real,
    'range_length_histogram', '{399,499,Infinity}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: range bounds histogram on scalar, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.31::real,
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
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'arange',
    'inherited', false::boolean,
    'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: cannot set most_common_elems for range type, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'arange',
    'inherited', false::boolean,
    'null_frac', 0.32::real,
    'most_common_elems', '{3,1}'::text,
    'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- warn: scalars can't have mcelem, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.33::real,
    'most_common_elems', '{1,3}'::text,
    'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- warn: mcelem / mcelem mismatch, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'tags',
    'inherited', false::boolean,
    'null_frac', 0.34::real,
    'most_common_elems', '{one,two}'::text
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- warn: mcelem / mcelem null mismatch part 2, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'tags',
    'inherited', false::boolean,
    'null_frac', 0.35::real,
    'most_common_elem_freqs', '{0.3,0.2,0.2,0.3}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- ok: mcelem
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'tags',
    'inherited', false::boolean,
    'most_common_elems', '{one,three}'::text,
    'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'tags';

-- warn: scalars can't have elem_count_histogram, rest ok
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.36::real,
    'elem_count_histogram', '{1,1,1,1,1,1,1,1,1,1}'::real[]
    );

SELECT *
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'id';

-- test for multiranges
INSERT INTO stats_import.test_mr
VALUES
  (1, 'red', '{[1,3),[5,9),[20,30)}'::int4multirange),
  (2, 'red', '{[11,13),[15,19),[20,30)}'::int4multirange),
  (3, 'red', '{[21,23),[25,29),[120,130)}'::int4multirange);

-- ensure that we set attribute stats for a multirange
SELECT pg_catalog.pg_restore_attribute_stats(
  'schemaname', 'stats_import',
  'relname', 'test_mr',
  'attname', 'mrange',
  'inherited', false,
  'range_length_histogram', '{19,29,109}'::text,
  'range_empty_frac', '0'::real,
  'range_bounds_histogram', '{"[1,30)","[11,30)","[21,130)"}'::text
);

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

CREATE STATISTICS stats_import.test_stat
  ON name, comp, lower(arange), array_length(tags,1)
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_ndistinct (ndistinct)
  ON name, comp
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_dependencies (dependencies)
  ON name, comp
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_mcv (mcv)
  ON name, comp
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_ndistinct_exprs (ndistinct)
  ON lower(name), upper(name)
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_dependencies_exprs (dependencies)
  ON lower(name), upper(name)
  FROM stats_import.test;

CREATE STATISTICS stats_import.test_stat_mcv_exprs (mcv)
  ON lower(name), upper(name)
  FROM stats_import.test;

-- Generate statistics on table with data
ANALYZE stats_import.test;

CREATE TABLE stats_import.test_clone ( LIKE stats_import.test )
    WITH (autovacuum_enabled = false);

CREATE INDEX is_odd_clone ON stats_import.test_clone(((comp).a % 2 = 1));

CREATE STATISTICS stats_import.test_stat_clone
  ON name, comp, lower(arange), array_length(tags,1)
  FROM stats_import.test_clone;

--
-- Copy stats from test to test_clone, and is_odd to is_odd_clone
--
SELECT s.schemaname, s.tablename, s.attname, s.inherited, r.*
FROM pg_catalog.pg_stats AS s
CROSS JOIN LATERAL
    pg_catalog.pg_restore_attribute_stats(
        'schemaname', 'stats_import',
        'relname', s.tablename::text || '_clone',
        'attname', s.attname::text,
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

-- attribute stats exist before a clear, but not after
SELECT COUNT(*)
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

SELECT pg_catalog.pg_clear_attribute_stats(
    schemaname => 'stats_import',
    relname => 'test',
    attname => 'arange',
    inherited => false);

SELECT COUNT(*)
FROM pg_stats
WHERE schemaname = 'stats_import'
AND tablename = 'test'
AND inherited = false
AND attname = 'arange';

-- temp tables
CREATE TEMP TABLE stats_temp(i int);
SELECT pg_restore_relation_stats(
        'schemaname', 'pg_temp',
        'relname', 'stats_temp',
        'relpages', '-19'::integer,
        'reltuples', 401::real,
        'relallvisible', 5::integer,
        'relallfrozen', 3::integer);

SELECT relname, relpages, reltuples, relallvisible, relallfrozen
FROM pg_class
WHERE oid = 'pg_temp.stats_temp'::regclass
ORDER BY relname;

SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'pg_temp',
    'relname', 'stats_temp',
    'attname', 'i',
    'inherited', false::boolean,
    'null_frac', 0.0123::real
    );

SELECT tablename, null_frac
FROM pg_stats
WHERE schemaname like 'pg_temp%'
AND tablename = 'stats_temp'
AND inherited = false
AND attname = 'i';
DROP TABLE stats_temp;

-- Tests for pg_clear_extended_stats().
--  Invalid argument values.
SELECT pg_clear_extended_stats(schemaname => NULL,
  relname => 'rel_foo',
  statistics_schemaname => 'schema_foo',
  statistics_name => 'stat_bar',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'schema_foo',
  relname => NULL,
  statistics_schemaname => 'schema_foo',
  statistics_name => 'stat_bar',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'schema_foo',
  relname => 'rel_foo',
  statistics_schemaname => NULL,
  statistics_name => 'stat_bar',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'schema_foo',
  relname => 'rel_foo',
  statistics_schemaname => 'schema_foo',
  statistics_name => NULL,
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'schema_foo',
  relname => 'rel_foo',
  statistics_schemaname => 'schema_foo',
  statistics_name => 'stat_bar',
  inherited => NULL);
-- Missing objects
SELECT pg_clear_extended_stats(schemaname => 'schema_not_exist',
  relname => 'test',
  statistics_schemaname => 'schema_not_exist',
  statistics_name => 'test_stat',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'stats_import',
  relname => 'table_not_exist',
  statistics_schemaname => 'stats_import',
  statistics_name => 'test_stat',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'schema_not_exist',
  statistics_name => 'test_stat',
  inherited => false);
SELECT pg_clear_extended_stats(schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'stats_import',
  statistics_name => 'ext_stats_not_exist',
  inherited => false);
-- Incorrect relation/extended stats combination
SELECT pg_clear_extended_stats(schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'stats_import',
  statistics_name => 'test_stat_clone',
  inherited => false);

-- Check that records are removed after a valid clear call.
SELECT COUNT(*), e.inherited FROM pg_stats_ext AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'test_stat' GROUP BY e.inherited;
SELECT COUNT(*), e.inherited FROM pg_stats_ext_exprs AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'test_stat' GROUP BY e.inherited;
BEGIN;
SELECT pg_catalog.pg_clear_extended_stats(
  schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'stats_import',
  statistics_name => 'test_stat',
  inherited => false);
SELECT mode FROM pg_locks WHERE locktype = 'relation' AND
  relation = 'stats_import.test'::regclass AND
  pid = pg_backend_pid();
COMMIT;
SELECT COUNT(*), e.inherited FROM pg_stats_ext AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'test_stat' GROUP BY e.inherited;
SELECT COUNT(*), e.inherited FROM pg_stats_ext_exprs AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'test_stat' GROUP BY e.inherited;
-- And before/after on inherited stats
SELECT COUNT(*), e.inherited FROM pg_stats_ext AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'part_parent_stat' GROUP BY e.inherited;
SELECT pg_catalog.pg_clear_extended_stats(
  schemaname => 'stats_import',
  relname => 'part_parent',
  statistics_schemaname => 'stats_import',
  statistics_name => 'part_parent_stat',
  inherited => true);
SELECT COUNT(*), e.inherited FROM pg_stats_ext AS e
  WHERE e.statistics_schemaname = 'stats_import' AND
  e.statistics_name = 'part_parent_stat' GROUP BY e.inherited;

-- Check that MAINTAIN is required when clearing statistics.
CREATE ROLE regress_test_extstat_clear;
GRANT ALL ON SCHEMA stats_import TO regress_test_extstat_clear;
SET ROLE regress_test_extstat_clear;
SELECT pg_catalog.pg_clear_extended_stats(
  schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'stats_import',
  statistics_name => 'test_stat',
  inherited => false);
RESET ROLE;
GRANT MAINTAIN ON stats_import.test TO regress_test_extstat_clear;
SET ROLE regress_test_extstat_clear;
SELECT pg_catalog.pg_clear_extended_stats(
  schemaname => 'stats_import',
  relname => 'test',
  statistics_schemaname => 'stats_import',
  statistics_name => 'test_stat',
  inherited => false);
RESET ROLE;
REVOKE MAINTAIN ON stats_import.test FROM regress_test_extstat_clear;
REVOKE ALL ON SCHEMA stats_import FROM regress_test_extstat_clear;
DROP ROLE regress_test_extstat_clear;

-- Tests for pg_restore_extended_stats().
--  Invalid argument values.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', NULL,
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', NULL,
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', NULL,
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', NULL,
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', NULL);
-- Missing objects
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'schema_not_exist',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'table_not_exist',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'schema_not_exist',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'ext_stats_not_exist',
  'inherited', false);
-- Incorrect relation/extended stats combination
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);

-- Check that MAINTAIN is required when restoring statistics.
CREATE ROLE regress_test_extstat_restore;
GRANT ALL ON SCHEMA stats_import TO regress_test_extstat_restore;
SET ROLE regress_test_extstat_restore;
-- No data to restore; this fails on a permission failure.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false);
RESET ROLE;
GRANT MAINTAIN ON stats_import.test_clone TO regress_test_extstat_restore;
SET ROLE regress_test_extstat_restore;
-- This works, check the lock on the relation while on it.
BEGIN;
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_clone',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_clone',
  'inherited', false,
  'n_distinct', '[{"attributes" : [2,3], "ndistinct" : 4}]'::pg_ndistinct);
SELECT mode FROM pg_locks WHERE locktype = 'relation' AND
  relation = 'stats_import.test_clone'::regclass AND
  pid = pg_backend_pid();
COMMIT;
RESET ROLE;
REVOKE MAINTAIN ON stats_import.test_clone FROM regress_test_extstat_restore;
REVOKE ALL ON SCHEMA stats_import FROM regress_test_extstat_restore;
DROP ROLE regress_test_extstat_restore;

-- ndistinct value doesn't match object definition
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_ndistinct',
  'inherited', false,
  'n_distinct', '[{"attributes" : [1,3], "ndistinct" : 4}]'::pg_ndistinct);
-- Incorrect extended stats kind, ndistinct not supported
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies',
  'inherited', false,
  'n_distinct', '[{"attributes" : [1,3], "ndistinct" : 4}]'::pg_ndistinct);
-- Incorrect extended stats kind, dependencies not supported
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_ndistinct',
  'inherited', false,
  'dependencies', '[{"attributes": [2], "dependency": 3, "degree": 1.000000},
                    {"attributes": [3], "dependency": 2, "degree": 1.000000}]'::pg_dependencies);

-- ok: ndistinct
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_ndistinct',
  'inherited', false,
  'n_distinct', '[{"attributes" : [2,3], "ndistinct" : 4}]'::pg_ndistinct);

-- dependencies value doesn't match definition
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies',
  'inherited', false,
  'dependencies', '[{"attributes": [1], "dependency": 3, "degree": 1.000000},
                    {"attributes": [3], "dependency": 1, "degree": 1.000000}]'::pg_dependencies);

-- ok: dependencies
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies',
  'inherited', false,
  'dependencies', '[{"attributes": [2], "dependency": 3, "degree": 1.000000},
                    {"attributes": [3], "dependency": 2, "degree": 1.000000}]'::pg_dependencies);

-- ndistinct with expressions, invalid attributes.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_ndistinct_exprs',
  'inherited', false,
  'n_distinct', '[{"attributes" : [1,-1], "ndistinct" : 4}]'::pg_ndistinct);

-- ok: ndistinct with expressions.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_ndistinct_exprs',
  'inherited', false,
  'n_distinct', '[{"attributes" : [-1,-2], "ndistinct" : 4}]'::pg_ndistinct);

-- dependencies with expressions, invalid attributes.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies_exprs',
  'inherited', false,
  'dependencies', '[{"attributes": [-1], "dependency": 1, "degree": 1.000000},
                    {"attributes": [1], "dependency": -1, "degree": 1.000000}]'::pg_dependencies);

-- ok: dependencies with expressions
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies_exprs',
  'inherited', false,
  'dependencies', '[{"attributes": [-1], "dependency": -2, "degree": 1.000000},
                    {"attributes": [-2], "dependency": -1, "degree": 1.000000}]'::pg_dependencies);

-- ok: MCV with expressions
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv_exprs',
  'inherited', false,
  'most_common_vals', '{{four,FOUR},{one,NULL},{NULL,TRE},{two,TWO}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.99}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.023,0.087}'::double precision[]);

-- Check the presence of the restored stats, for each object.
SELECT replace(e.n_distinct,   '}, ', E'},\n') AS n_distinct
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_ndistinct' AND
    e.inherited = false;

SELECT replace(e.dependencies, '}, ', E'},\n') AS dependencies
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_dependencies' AND
    e.inherited = false;

SELECT replace(e.n_distinct,   '}, ', E'},\n') AS n_distinct
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_ndistinct_exprs' AND
    e.inherited = false;

SELECT replace(e.dependencies, '}, ', E'},\n') AS dependencies
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_dependencies_exprs' AND
    e.inherited = false;

SELECT e.most_common_vals, e.most_common_val_nulls,
       e.most_common_freqs, e.most_common_base_freqs
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_mcv_exprs' AND
    e.inherited = false \gx

-- Incorrect extended stats kind, mcv not supported
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_dependencies',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);

-- MCV requires all three parameters
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[]);

-- most_common_vals that is not 2-D
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{four,NULL}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);

-- most_common_freqs with length not matching with most_common_vals.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);

-- most_common_base_freqs with length not matching most_common_vals.
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625}'::double precision[]);

-- mcv attributes not matching object definition
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL,0,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")",1,2},
                        {tre,"(3,3.3,TRE,03-03-2003,)",-1,3},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")",1,2}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.00390625,0.015625,0.00390625,0.015625}'::double precision[]);

-- ok: mcv
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_stat_mcv',
  'inherited', false,
  'most_common_vals', '{{four,NULL},
                        {one,"(1,1.1,ONE,01-01-2001,\"{\"\"xkey\"\": \"\"xval\"\"}\")"},
                        {tre,"(3,3.3,TRE,03-03-2003,)"},
                        {two,"(2,2.2,TWO,02-02-2002,\"[true, 4, \"\"six\"\"]\")"}}'::text[],
  'most_common_freqs', '{0.25,0.25,0.25,0.25}'::double precision[],
  'most_common_base_freqs', '{0.0625,0.0625,0.0625,0.0625}'::double precision[]);

SELECT replace(e.most_common_vals::text, '},', E'},\n ') AS mcvs,
       e.most_common_val_nulls,
       e.most_common_freqs, e.most_common_base_freqs
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_stat_mcv' AND
    e.inherited = false
\gx

-- Check import of all kinds for multirange.
CREATE STATISTICS stats_import.test_mr_stat
  ON name, mrange, ( mrange + '{[10000,10200)}'::int4multirange)
  FROM stats_import.test_mr;

-- ok: multirange stats
SELECT pg_catalog.pg_restore_extended_stats(
  'schemaname', 'stats_import',
  'relname', 'test_mr',
  'statistics_schemaname', 'stats_import',
  'statistics_name', 'test_mr_stat',
  'inherited', false,
  'n_distinct', '[{"attributes": [2, 3], "ndistinct": 3},
                  {"attributes": [2, -1], "ndistinct": 3},
                  {"attributes": [3, -1], "ndistinct": 3},
                  {"attributes": [2, 3, -1], "ndistinct": 3}]'::pg_catalog.pg_ndistinct,
  'dependencies', '[{"attributes": [3], "dependency": 2, "degree": 1.000000},
                    {"attributes": [3], "dependency": -1, "degree": 1.000000},
                    {"attributes": [-1], "dependency": 2, "degree": 1.000000},
                    {"attributes": [-1], "dependency": 3, "degree": 1.000000},
                    {"attributes": [2, 3], "dependency": -1, "degree": 1.000000},
                    {"attributes": [2, -1], "dependency": 3, "degree": 1.000000},
                    {"attributes": [3, -1], "dependency": 2, "degree": 1.000000}]'::pg_catalog.pg_dependencies,
  'most_common_vals', '{{red,"{[1,3),[5,9),[20,30)}","{[1,3),[5,9),[20,30),[10000,10200)}"},
                        {red,"{[11,13),[15,19),[20,30)}","{[11,13),[15,19),[20,30),[10000,10200)}"},
                        {red,"{[21,23),[25,29),[120,130)}","{[21,23),[25,29),[120,130),[10000,10200)}"}}'::text[],
  'most_common_freqs', '{0.3333333333333333,0.3333333333333333,0.3333333333333333}'::double precision[],
  'most_common_base_freqs', '{0.1111111111111111,0.1111111111111111,0.1111111111111111}'::double precision[]
);

SELECT replace(e.n_distinct,   '}, ', E'},\n') AS n_distinct,
       replace(e.dependencies, '}, ', E'},\n') AS dependencies,
       replace(e.most_common_vals::text, '},', E'},\n ') AS mcvs,
       e.most_common_val_nulls,
       e.most_common_freqs, e.most_common_base_freqs
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import' AND
    e.statistics_name = 'test_mr_stat' AND
    e.inherited = false
\gx

-- Test the ability of pg_restore_extended_stats() to import all of the
-- statistic values from an extended statistic object that has been
-- populated via a regular ANALYZE.  This checks after the statistics
-- kinds supported by pg_restore_extended_stats().
--
-- Note: Keep this test at the bottom of the file, so as the amount of
-- statistics data handled is maximized.
ANALYZE stats_import.test;

-- Copy stats from test_stat to test_stat_clone
SELECT e.statistics_name,
  pg_catalog.pg_restore_extended_stats(
    'schemaname', e.statistics_schemaname::text,
    'relname', 'test_clone',
    'statistics_schemaname', e.statistics_schemaname::text,
    'statistics_name', 'test_stat_clone',
    'inherited', e.inherited,
    'n_distinct', e.n_distinct,
    'dependencies', e.dependencies,
    'most_common_vals', e.most_common_vals,
    'most_common_freqs', e.most_common_freqs,
    'most_common_base_freqs', e.most_common_base_freqs)
FROM pg_stats_ext AS e
WHERE e.statistics_schemaname = 'stats_import'
AND e.statistics_name = 'test_stat';

-- Set difference old MINUS new.
SELECT o.inherited,
       o.n_distinct, o.dependencies, o.most_common_vals,
       o.most_common_freqs, o.most_common_base_freqs
  FROM pg_stats_ext AS o
  WHERE o.statistics_schemaname = 'stats_import' AND
    o.statistics_name = 'test_stat'
EXCEPT
SELECT n.inherited,
       n.n_distinct, n.dependencies, n.most_common_vals,
       n.most_common_freqs, n.most_common_base_freqs
  FROM pg_stats_ext AS n
  WHERE n.statistics_schemaname = 'stats_import' AND
    n.statistics_name = 'test_stat_clone';
-- Set difference new MINUS old.
SELECT n.inherited,
       n.n_distinct, n.dependencies, n.most_common_vals,
       n.most_common_freqs, n.most_common_base_freqs
  FROM pg_stats_ext AS n
  WHERE n.statistics_schemaname = 'stats_import' AND
    n.statistics_name = 'test_stat_clone'
EXCEPT
SELECT o.inherited,
       o.n_distinct, o.dependencies, o.most_common_vals,
       o.most_common_freqs, o.most_common_base_freqs
  FROM pg_stats_ext AS o
  WHERE o.statistics_schemaname = 'stats_import' AND
    o.statistics_name = 'test_stat';

DROP SCHEMA stats_import CASCADE;
