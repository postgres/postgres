-- Tests with various custom types

CREATE EXTENSION test_custom_types;

-- Test comparison functions
SELECT '42'::int_custom = '42'::int_custom AS eq_test;
SELECT '42'::int_custom <> '42'::int_custom AS nt_test;
SELECT '42'::int_custom < '100'::int_custom AS lt_test;
SELECT '100'::int_custom > '42'::int_custom AS gt_test;
SELECT '42'::int_custom <= '100'::int_custom AS le_test;
SELECT '100'::int_custom >= '42'::int_custom AS ge_test;

-- Create a table with the int_custom type
CREATE TABLE test_table (
    id int,
    data int_custom
);
INSERT INTO test_table VALUES (1, '42'), (2, '100'), (3, '200');

-- Verify data was inserted correctly
SELECT * FROM test_table ORDER BY id;

-- Dummy function used for expression evaluations.
-- Note that this function does not use a SQL-standard function body on
-- purpose, so as external statistics can be loaded from it.
CREATE OR REPLACE FUNCTION func_int_custom (p_value int_custom)
  RETURNS int_custom LANGUAGE plpgsql AS $$
  BEGIN
    RETURN p_value;
  END; $$;

-- Switch type to use typanalyze function that always returns false.
ALTER TYPE int_custom SET (ANALYZE = int_custom_typanalyze_false);

-- Extended statistics with an attribute that cannot be analyzed.
-- This includes all statistics kinds.
CREATE STATISTICS test_stats ON data, id FROM test_table;
-- Computation of the stats fails, no data generated.
ANALYZE test_table;
SELECT stxname, stxdexpr IS NULL as expr_stats_is_null
  FROM pg_statistic_ext s
  LEFT JOIN pg_statistic_ext_data d ON s.oid = d.stxoid
  WHERE stxname = 'test_stats';
DROP STATISTICS test_stats;

-- Extended statistics with an expression that cannot be analyzed.
CREATE STATISTICS test_stats ON func_int_custom(data), (id) FROM test_table;
-- Computation of the stats fails, no data generated.
ANALYZE test_table;
SELECT stxname, stxdexpr IS NULL as expr_stats_is_null
  FROM pg_statistic_ext s
  LEFT JOIN pg_statistic_ext_data d ON s.oid = d.stxoid
  WHERE stxname = 'test_stats';
DROP STATISTICS test_stats;
-- There should be no data stored for the expression.
SELECT tablename,
  statistics_name,
  null_frac,
  avg_width
  FROM pg_stats_ext_exprs WHERE statistics_name = 'test_stats' \gx

-- Switch type to use typanalyze function that generates invalid data.
ALTER TYPE int_custom SET (ANALYZE = int_custom_typanalyze_invalid);

-- Extended statistics with an attribute that generates invalid stats.
CREATE STATISTICS test_stats ON data, id FROM test_table;
-- Computation of the stats fails, no data generated.
ANALYZE test_table;
SELECT stxname, stxdexpr IS NULL as expr_stats_is_null
  FROM pg_statistic_ext s
  LEFT JOIN pg_statistic_ext_data d ON s.oid = d.stxoid
  WHERE stxname = 'test_stats';
DROP STATISTICS test_stats;

-- Extended statistics with an expression that generates invalid data.
CREATE STATISTICS test_stats ON func_int_custom(data), (id) FROM test_table;
-- Computation of the stats fails, some data generated.
ANALYZE test_table;
SELECT stxname, stxdexpr IS NULL as expr_stats_is_null
  FROM pg_statistic_ext s
  LEFT JOIN pg_statistic_ext_data d ON s.oid = d.stxoid
  WHERE stxname = 'test_stats';
-- There should be some data stored for the expression, stored as NULL.
SELECT tablename,
  statistics_name,
  null_frac,
  avg_width,
  n_distinct,
  most_common_vals,
  most_common_freqs,
  histogram_bounds,
  correlation,
  most_common_elems,
  most_common_elem_freqs,
  elem_count_histogram,
  range_length_histogram,
  range_empty_frac,
  range_bounds_histogram
  FROM pg_stats_ext_exprs WHERE statistics_name = 'test_stats' \gx
-- Run a query able to load the extended stats, including the NULL data.
SELECT COUNT(*) FROM test_table GROUP BY (func_int_custom(data));
DROP STATISTICS test_stats;

-- Cleanup
DROP FUNCTION func_int_custom;
DROP TABLE test_table;
DROP EXTENSION test_custom_types;
