--
-- PARTITIONS_MERGE
-- Tests for "ALTER TABLE ... MERGE PARTITIONS ..." command
--

CREATE SCHEMA partitions_merge_schema;
CREATE SCHEMA partitions_merge_schema2;
SET search_path = partitions_merge_schema, public;

--
-- BY RANGE partitioning
--

--
-- Test for error codes
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_dec2021 PARTITION OF sales_range FOR VALUES FROM ('2021-12-01') TO ('2021-12-31');
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE sales_mar2022 PARTITION OF sales_range FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');

CREATE TABLE sales_apr2022 (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_apr_1 PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-04-15');
CREATE TABLE sales_apr_2 PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-15') TO ('2022-05-01');
ALTER TABLE sales_range ATTACH PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');

CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

-- ERROR:  partition with name "sales_feb2022" is already used
ALTER TABLE sales_range MERGE PARTITIONS (sales_feb2022, sales_mar2022, sales_feb2022) INTO sales_feb_mar_apr2022;
-- ERROR:  "sales_apr2022" is not a table
ALTER TABLE sales_range MERGE PARTITIONS (sales_feb2022, sales_mar2022, sales_apr2022) INTO sales_feb_mar_apr2022;
-- ERROR:  can not merge partition "sales_mar2022" together with partition "sales_jan2022"
-- DETAIL:  lower bound of partition "sales_mar2022" is not equal to the upper bound of partition "sales_jan2022"
-- (space between sections sales_jan2022 and sales_mar2022)
ALTER TABLE sales_range MERGE PARTITIONS (sales_jan2022, sales_mar2022) INTO sales_jan_mar2022;
-- ERROR:  can not merge partition "sales_jan2022" together with partition "sales_dec2021"
-- DETAIL:  lower bound of partition "sales_jan2022" is not equal to the upper bound of partition "sales_dec2021"
-- (space between sections sales_dec2021 and sales_jan2022)
ALTER TABLE sales_range MERGE PARTITIONS (sales_dec2021, sales_jan2022, sales_feb2022) INTO sales_dec_jan_feb2022;
-- ERROR:  partition with name "sales_feb2022" is already used
ALTER TABLE sales_range MERGE PARTITIONS (sales_feb2022, sales_mar2022, partitions_merge_schema.sales_feb2022) INTO sales_feb_mar_apr2022;
--ERROR, sales_apr_2 already exists
ALTER TABLE sales_range MERGE PARTITIONS (sales_feb2022, sales_mar2022, sales_jan2022) INTO sales_apr_2;

CREATE VIEW jan2022v as SELECT * FROM sales_jan2022;
ALTER TABLE sales_range MERGE PARTITIONS (sales_jan2022, sales_feb2022) INTO sales_dec_jan_feb2022;
DROP VIEW jan2022v;

-- NO ERROR: test for custom partitions order, source partitions not in the search_path
SET search_path = partitions_merge_schema2, public;
ALTER TABLE partitions_merge_schema.sales_range MERGE PARTITIONS (
  partitions_merge_schema.sales_feb2022,
  partitions_merge_schema.sales_mar2022,
  partitions_merge_schema.sales_jan2022) INTO sales_jan_feb_mar2022;
SET search_path = partitions_merge_schema, public;

PREPARE get_partition_info(regclass[]) AS
SELECT  c.oid::pg_catalog.regclass,
        c.relpersistence,
        c.relkind,
        i.inhdetachpending,
        pg_catalog.pg_get_expr(c.relpartbound, c.oid)
FROM    pg_catalog.pg_class c, pg_catalog.pg_inherits i
WHERE   c.oid = i.inhrelid AND i.inhparent = ANY($1)
ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT',
         c.oid::regclass::text COLLATE "C";

EXECUTE get_partition_info('{sales_range}');

DROP TABLE sales_range;

--
-- Add rows into partitioned table, then merge partitions
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE sales_mar2022 PARTITION OF sales_range FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');
CREATE TABLE sales_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;
CREATE INDEX sales_range_sales_date_idx ON sales_range USING btree (sales_date);

INSERT INTO sales_range VALUES
  (1,  'May',      1000, '2022-01-31'),
  (2,  'Smirnoff', 500,  '2022-02-10'),
  (3,  'Ford',     2000, '2022-04-30'),
  (4,  'Ivanov',   750,  '2022-04-13'),
  (5,  'Deev',     250,  '2022-04-07'),
  (6,  'Poirot',   150,  '2022-02-11'),
  (7,  'Li',       175,  '2022-03-08'),
  (8,  'Ericsson', 185,  '2022-02-23'),
  (9,  'Muller',   250,  '2022-03-11'),
  (10, 'Halder',   350,  '2022-01-28'),
  (11, 'Trump',    380,  '2022-04-06'),
  (12, 'Plato',    350,  '2022-03-19'),
  (13, 'Gandi',    377,  '2022-01-09'),
  (14, 'Smith',    510,  '2022-05-04');

SELECT pg_catalog.pg_get_partkeydef('sales_range'::regclass);

-- show partitions with conditions:
EXECUTE get_partition_info('{sales_range}');

-- check schema-qualified name of the new partition
ALTER TABLE sales_range MERGE PARTITIONS (sales_feb2022, sales_mar2022, sales_apr2022) INTO partitions_merge_schema2.sales_feb_mar_apr2022;

-- show partitions with conditions:
EXECUTE get_partition_info('{sales_range}');

SELECT * FROM pg_indexes WHERE tablename = 'sales_feb_mar_apr2022' and schemaname = 'partitions_merge_schema2';

SELECT tableoid::regclass, * FROM sales_range ORDER BY tableoid::regclass::text COLLATE "C", salesperson_id;

-- Use indexscan for testing indexes
SET enable_seqscan = OFF;

EXPLAIN (COSTS OFF) SELECT * FROM partitions_merge_schema2.sales_feb_mar_apr2022 where sales_date > '2022-01-01';
SELECT * FROM partitions_merge_schema2.sales_feb_mar_apr2022 where sales_date > '2022-01-01';

RESET enable_seqscan;

DROP TABLE sales_range;

--
-- Merge some partitions into DEFAULT partition
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE sales_mar2022 PARTITION OF sales_range FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');
CREATE TABLE sales_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;
CREATE INDEX sales_range_sales_date_idx ON sales_range USING btree (sales_date);

INSERT INTO sales_range VALUES
  (1,  'May',      1000, '2022-01-31'),
  (2,  'Smirnoff', 500,  '2022-02-10'),
  (3,  'Ford',     2000, '2022-04-30'),
  (4,  'Ivanov',   750,  '2022-04-13'),
  (5,  'Deev',     250,  '2022-04-07'),
  (6,  'Poirot',   150,  '2022-02-11'),
  (7,  'Li',       175,  '2022-03-08'),
  (8,  'Ericsson', 185,  '2022-02-23'),
  (9,  'Muller',   250,  '2022-03-11'),
  (10, 'Halder',   350,  '2022-01-28'),
  (11, 'Trump',    380,  '2022-04-06'),
  (12, 'Plato',    350,  '2022-03-19'),
  (13, 'Gandi',    377,  '2022-01-09'),
  (14, 'Smith',    510,  '2022-05-04');

-- Merge partitions (include DEFAULT partition) into partition with the same
-- name
ALTER TABLE sales_range MERGE PARTITIONS
  (sales_jan2022, sales_mar2022, partitions_merge_schema.sales_others) INTO sales_others;

SELECT * FROM sales_others ORDER BY salesperson_id;

-- show partitions with conditions:
EXECUTE get_partition_info('{sales_range}');

DROP TABLE sales_range;

--
-- Test for:
--   * composite partition key;
--   * GENERATED column;
--   * column with DEFAULT value.
--
CREATE TABLE sales_date (salesperson_name VARCHAR(30), sales_year INT, sales_month INT, sales_day INT,
  sales_date VARCHAR(10) GENERATED ALWAYS AS
    (LPAD(sales_year::text, 4, '0') || '.' || LPAD(sales_month::text, 2, '0') || '.' || LPAD(sales_day::text, 2, '0')) STORED,
  sales_department VARCHAR(30) DEFAULT 'Sales department')
  PARTITION BY RANGE (sales_year, sales_month, sales_day);

CREATE TABLE sales_dec2022 PARTITION OF sales_date FOR VALUES FROM (2021, 12, 1) TO (2022, 1, 1);
CREATE TABLE sales_jan2022 PARTITION OF sales_date FOR VALUES FROM (2022, 1, 1) TO (2022, 2, 1);
CREATE TABLE sales_feb2022 PARTITION OF sales_date FOR VALUES FROM (2022, 2, 1) TO (2022, 3, 1);
CREATE TABLE sales_other PARTITION OF sales_date FOR VALUES FROM (2022, 3, 1) TO (MAXVALUE, MAXVALUE, MAXVALUE);

INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES
  ('Manager1', 2021, 12, 7),
  ('Manager2', 2021, 12, 8),
  ('Manager3', 2022, 1, 1),
  ('Manager1', 2022, 2, 4),
  ('Manager2', 2022, 1, 2),
  ('Manager3', 2022, 2, 1),
  ('Manager1', 2022, 3, 3),
  ('Manager2', 2022, 3, 4),
  ('Manager3', 2022, 5, 1);

SELECT tableoid::regclass, * FROM sales_date;

ALTER TABLE sales_date MERGE PARTITIONS (sales_jan2022, sales_feb2022) INTO sales_jan_feb2022;

INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES
  ('Manager1', 2022, 1, 10),
  ('Manager2', 2022, 2, 10);

SELECT tableoid::regclass, * FROM sales_date;
DROP TABLE sales_date;

--
-- Test: merge partitions of partitioned table with triggers
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);

CREATE TABLE salespeople01_10 PARTITION OF salespeople FOR VALUES FROM (1) TO (10);
CREATE TABLE salespeople10_20 PARTITION OF salespeople FOR VALUES FROM (10) TO (20);
CREATE TABLE salespeople20_30 PARTITION OF salespeople FOR VALUES FROM (20) TO (30);
CREATE TABLE salespeople30_40 PARTITION OF salespeople FOR VALUES FROM (30) TO (40);

INSERT INTO salespeople VALUES (1,  'Poirot');

CREATE OR REPLACE FUNCTION after_insert_row_trigger() RETURNS trigger LANGUAGE 'plpgsql' AS $BODY$
BEGIN
    RAISE NOTICE 'trigger(%) called: action = %, when = %, level = %', TG_ARGV[0], TG_OP, TG_WHEN, TG_LEVEL;
    RETURN NULL;
END;
$BODY$;

CREATE TRIGGER salespeople_after_insert_statement_trigger
    AFTER INSERT
    ON salespeople
    FOR EACH STATEMENT
    EXECUTE PROCEDURE after_insert_row_trigger('salespeople');

CREATE TRIGGER salespeople_after_insert_row_trigger
    AFTER INSERT
    ON salespeople
    FOR EACH ROW
    EXECUTE PROCEDURE after_insert_row_trigger('salespeople');

-- 2 triggers should fire here (row + statement):
INSERT INTO salespeople VALUES (10, 'May');
-- 1 trigger should fire here (row):
INSERT INTO salespeople10_20 VALUES (19, 'Ivanov');

ALTER TABLE salespeople MERGE PARTITIONS (salespeople10_20, salespeople20_30, salespeople30_40) INTO salespeople10_40;

-- 2 triggers should fire here (row + statement):
INSERT INTO salespeople VALUES (20, 'Smirnoff');
-- 1 trigger should fire here (row):
INSERT INTO salespeople10_40 VALUES (30, 'Ford');

SELECT * FROM salespeople01_10;
SELECT * FROM salespeople10_40;

DROP TABLE salespeople;
DROP FUNCTION after_insert_row_trigger();

--
-- Test: merge partitions with deleted columns
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);

CREATE TABLE salespeople01_10 PARTITION OF salespeople FOR VALUES FROM (1) TO (10);
-- Create partitions with some deleted columns:
CREATE TABLE salespeople10_20(d1 VARCHAR(30), salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30));
CREATE TABLE salespeople20_30(salesperson_id INT PRIMARY KEY, d2 INT,  salesperson_name VARCHAR(30));
CREATE TABLE salespeople30_40(salesperson_id INT PRIMARY KEY, d3 DATE, salesperson_name VARCHAR(30));

INSERT INTO salespeople10_20 VALUES ('dummy value 1', 19, 'Ivanov');
INSERT INTO salespeople20_30 VALUES (20, 101, 'Smirnoff');
INSERT INTO salespeople30_40 VALUES (31, now(), 'Popov');

ALTER TABLE salespeople10_20 DROP COLUMN d1;
ALTER TABLE salespeople20_30 DROP COLUMN d2;
ALTER TABLE salespeople30_40 DROP COLUMN d3;

ALTER TABLE salespeople ATTACH PARTITION salespeople10_20 FOR VALUES FROM (10) TO (20);
ALTER TABLE salespeople ATTACH PARTITION salespeople20_30 FOR VALUES FROM (20) TO (30);
ALTER TABLE salespeople ATTACH PARTITION salespeople30_40 FOR VALUES FROM (30) TO (40);

INSERT INTO salespeople VALUES
  (1, 'Poirot'),
  (10, 'May'),
  (30, 'Ford');

ALTER TABLE salespeople MERGE PARTITIONS (salespeople10_20, salespeople20_30, salespeople30_40) INTO salespeople10_40;

select * from salespeople;
select * from salespeople01_10;
select * from salespeople10_40;

DROP TABLE salespeople;

--
-- Test: merge sub-partitions
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE sales_mar2022 PARTITION OF sales_range FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');

CREATE TABLE sales_apr2022 (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_apr2022_01_10 PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-04-10');
CREATE TABLE sales_apr2022_10_20 PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-10') TO ('2022-04-20');
CREATE TABLE sales_apr2022_20_30 PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-20') TO ('2022-05-01');
ALTER TABLE sales_range ATTACH PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');

CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

CREATE INDEX sales_range_sales_date_idx ON sales_range USING btree (sales_date);

INSERT INTO sales_range VALUES
  (1,  'May',      1000, '2022-01-31'),
  (2,  'Smirnoff', 500,  '2022-02-10'),
  (3,  'Ford',     2000, '2022-04-30'),
  (4,  'Ivanov',   750,  '2022-04-13'),
  (5,  'Deev',     250,  '2022-04-07'),
  (6,  'Poirot',   150,  '2022-02-11'),
  (7,  'Li',       175,  '2022-03-08'),
  (8,  'Ericsson', 185,  '2022-02-23'),
  (9,  'Muller',   250,  '2022-03-11'),
  (10, 'Halder',   350,  '2022-01-28'),
  (11, 'Trump',    380,  '2022-04-06'),
  (12, 'Plato',    350,  '2022-03-19'),
  (13, 'Gandi',    377,  '2022-01-09'),
  (14, 'Smith',    510,  '2022-05-04');

SELECT tableoid::regclass, * FROM sales_apr2022 ORDER BY tableoid::regclass::text COLLATE "C", salesperson_id;

ALTER TABLE sales_apr2022 MERGE PARTITIONS (sales_apr2022_01_10, sales_apr2022_10_20, sales_apr2022_20_30) INTO sales_apr_all;

SELECT tableoid::regclass, * FROM sales_apr2022 ORDER BY tableoid::regclass::text COLLATE "C", salesperson_id;

DROP TABLE sales_range;

--
-- BY LIST partitioning
--

--
-- Test: specific errors for BY LIST partitioning
--
CREATE TABLE sales_list
(salesperson_id INT GENERATED ALWAYS AS IDENTITY,
 salesperson_name VARCHAR(30),
 sales_state VARCHAR(20),
 sales_amount INT,
 sales_date DATE)
PARTITION BY LIST (sales_state);
CREATE TABLE sales_nord PARTITION OF sales_list FOR VALUES IN ('Oslo', 'St. Petersburg', 'Helsinki');
CREATE TABLE sales_west PARTITION OF sales_list FOR VALUES IN ('Lisbon', 'New York', 'Madrid');
CREATE TABLE sales_east PARTITION OF sales_list FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok');
CREATE TABLE sales_central PARTITION OF sales_list FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv');
CREATE TABLE sales_others PARTITION OF sales_list DEFAULT;


CREATE TABLE sales_list2 (LIKE sales_list) PARTITION BY LIST (sales_state);
CREATE TABLE sales_nord2 PARTITION OF sales_list2 FOR VALUES IN ('Oslo', 'St. Petersburg', 'Helsinki');
CREATE TABLE sales_others2 PARTITION OF sales_list2 DEFAULT;


CREATE TABLE sales_external (LIKE sales_list);
CREATE TABLE sales_external2 (vch VARCHAR(5));

-- ERROR:  "sales_external" is not a partition of partitioned table "sales_list"
ALTER TABLE sales_list MERGE PARTITIONS (sales_west, sales_east, sales_external) INTO sales_all;
-- ERROR:  "sales_external2" is not a partition of partitioned table "sales_list"
ALTER TABLE sales_list MERGE PARTITIONS (sales_west, sales_east, sales_external2) INTO sales_all;
-- ERROR:  relation "sales_nord2" is not a partition of relation "sales_list"
ALTER TABLE sales_list MERGE PARTITIONS (sales_west, sales_nord2, sales_east) INTO sales_all;

DROP TABLE sales_external2;
DROP TABLE sales_external;
DROP TABLE sales_list2;
DROP TABLE sales_list;

--
-- Test: BY LIST partitioning, MERGE PARTITIONS with data
--
CREATE TABLE sales_list
(salesperson_id INT GENERATED ALWAYS AS IDENTITY,
 salesperson_name VARCHAR(30),
 sales_state VARCHAR(20),
 sales_amount INT,
 sales_date DATE)
PARTITION BY LIST (sales_state);

CREATE INDEX sales_list_salesperson_name_idx ON sales_list USING btree (salesperson_name);
CREATE INDEX sales_list_sales_state_idx ON sales_list USING btree (sales_state);

CREATE TABLE sales_nord PARTITION OF sales_list FOR VALUES IN ('Oslo', 'St. Petersburg', 'Helsinki');
CREATE TABLE sales_west PARTITION OF sales_list FOR VALUES IN ('Lisbon', 'New York', 'Madrid');
CREATE TABLE sales_east PARTITION OF sales_list FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok');
CREATE TABLE sales_central PARTITION OF sales_list FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv');
CREATE TABLE sales_others PARTITION OF sales_list DEFAULT;

INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES
  ('Trump',    'Bejing',         1000, '2022-03-01'),
  ('Smirnoff', 'New York',        500, '2022-03-03'),
  ('Ford',     'St. Petersburg', 2000, '2022-03-05'),
  ('Ivanov',   'Warsaw',          750, '2022-03-04'),
  ('Deev',     'Lisbon',          250, '2022-03-07'),
  ('Poirot',   'Berlin',         1000, '2022-03-01'),
  ('May',      'Helsinki',       1200, '2022-03-06'),
  ('Li',       'Vladivostok',    1150, '2022-03-09'),
  ('May',      'Helsinki',       1200, '2022-03-11'),
  ('Halder',   'Oslo',            800, '2022-03-02'),
  ('Muller',   'Madrid',          650, '2022-03-05'),
  ('Smith',    'Kyiv',            350, '2022-03-10'),
  ('Gandi',    'Warsaw',          150, '2022-03-08'),
  ('Plato',    'Lisbon',          950, '2022-03-05');

-- show partitions with conditions:
EXECUTE get_partition_info('{sales_list}');

ALTER TABLE sales_list MERGE PARTITIONS (sales_west, sales_east, sales_central) INTO sales_all;

-- show partitions with conditions:
EXECUTE get_partition_info('{sales_list}');

SELECT tableoid::regclass, * FROM sales_list ORDER BY tableoid::regclass::text COLLATE "C", salesperson_id;

-- Use indexscan for testing indexes after merging partitions
SET enable_seqscan = OFF;

EXPLAIN (COSTS OFF) SELECT * FROM sales_all WHERE sales_state = 'Warsaw';
SELECT * FROM sales_all WHERE sales_state = 'Warsaw';
EXPLAIN (COSTS OFF) SELECT * FROM sales_list WHERE sales_state = 'Warsaw';
SELECT * FROM sales_list WHERE sales_state = 'Warsaw';
EXPLAIN (COSTS OFF) SELECT * FROM sales_list WHERE salesperson_name = 'Ivanov';
SELECT * FROM sales_list WHERE salesperson_name = 'Ivanov';

RESET enable_seqscan;

DROP TABLE sales_list;

--
-- Try to MERGE partitions of another table.
--
CREATE TABLE t1 (i int, a int, b int, c int) PARTITION BY RANGE (a, b);
CREATE TABLE t1p1 PARTITION OF t1 FOR VALUES FROM (1, 1) TO (1, 2);
CREATE TABLE t2 (i int, t text) PARTITION BY RANGE (t);
CREATE TABLE t2pa PARTITION OF t2 FOR VALUES FROM ('A') TO ('C');
CREATE TABLE t3 (i int, t text);

-- ERROR:  relation "t1p1" is not a partition of relation "t2"
ALTER TABLE t2 MERGE PARTITIONS (t1p1, t2pa) INTO t2p;
-- ERROR:  "t3" is not a partition of partitioned table "t2"
ALTER TABLE t2 MERGE PARTITIONS (t2pa, t3) INTO t2p;

DROP TABLE t3;
DROP TABLE t2;
DROP TABLE t1;


--
-- Check the partition index name if the partition name is the same as one
-- of the merged partitions.
--
CREATE TABLE t (i int, PRIMARY KEY(i)) PARTITION BY RANGE (i);

CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);

CREATE INDEX tidx ON t(i);
ALTER TABLE t MERGE PARTITIONS (tp_1_2, tp_0_1) INTO tp_1_2;

-- Indexname values should be 'tp_1_2_pkey' and 'tp_1_2_i_idx'.
\d+ tp_1_2

DROP TABLE t;

--
-- Try to MERGE partitions of temporary table.
--
BEGIN;
SHOW search_path;
CREATE TEMP TABLE t (i int) PARTITION BY RANGE (i) ON COMMIT DROP;
CREATE TEMP TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TEMP TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
CREATE TEMP TABLE tp_2_3 PARTITION OF t FOR VALUES FROM (2) TO (3);
CREATE TEMP TABLE tp_3_4 PARTITION OF t FOR VALUES FROM (3) TO (4);

ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO pg_temp.tp_0_2;
ALTER TABLE t MERGE PARTITIONS (tp_0_2, tp_2_3) INTO pg_temp.tp_0_3;

-- Partition should be temporary.
EXECUTE get_partition_info('{t}');
-- ERROR:  cannot create a permanent relation as partition of temporary relation "t"
ALTER TABLE t MERGE PARTITIONS (tp_0_3, tp_3_4) INTO tp_0_4;
ROLLBACK;

--
-- Try mixing permanent and temporary partitions.
--
BEGIN;
SET search_path = partitions_merge_schema, pg_temp, public;
CREATE TABLE t (i int) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);

SELECT c.oid::pg_catalog.regclass, c.relpersistence FROM pg_catalog.pg_class c WHERE c.oid = 't'::regclass;
EXECUTE get_partition_info('{t}');
SAVEPOINT s;

SET search_path = pg_temp, partitions_merge_schema, public;
-- Can't merge persistent partitions into a temporary partition
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;

ROLLBACK TO SAVEPOINT s;
SET search_path = partitions_merge_schema, public;
-- Can't merge persistent partitions into a temporary partition
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO pg_temp.tp_0_2;
ROLLBACK;

BEGIN;
SET search_path = pg_temp, partitions_merge_schema, public;
CREATE TABLE t (i int) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);

SELECT c.oid::pg_catalog.regclass, c.relpersistence FROM pg_catalog.pg_class c WHERE c.oid = 't'::regclass;
EXECUTE get_partition_info('{t}');

SET search_path = partitions_merge_schema, pg_temp, public;

-- Can't merge temporary partitions into a persistent partition
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
ROLLBACK;

DEALLOCATE get_partition_info;

-- Check the new partition inherits parent's tablespace
SET search_path = partitions_merge_schema, public;
CREATE TABLE t (i int PRIMARY KEY USING INDEX TABLESPACE regress_tblspace)
  PARTITION BY RANGE (i) TABLESPACE regress_tblspace;
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
SELECT tablename, tablespace FROM pg_tables
  WHERE tablename IN ('t', 'tp_0_2') AND schemaname = 'partitions_merge_schema'
  ORDER BY tablename COLLATE "C", tablespace COLLATE "C";
SELECT tablename, indexname, tablespace FROM pg_indexes
  WHERE tablename IN ('t', 'tp_0_2') AND schemaname = 'partitions_merge_schema'
  ORDER BY tablename COLLATE "C", indexname COLLATE "C", tablespace COLLATE "C";
DROP TABLE t;

-- Check the new partition inherits parent's table access method
SET search_path = partitions_merge_schema, public;
CREATE ACCESS METHOD partitions_merge_heap TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE t (i int) PARTITION BY RANGE (i) USING partitions_merge_heap;
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
SELECT c.relname, a.amname
FROM pg_class c JOIN pg_am a ON c.relam = a.oid
WHERE c.oid IN ('t'::regclass, 'tp_0_2'::regclass)
ORDER BY c.relname COLLATE "C";
DROP TABLE t;
DROP ACCESS METHOD partitions_merge_heap;

-- Test permission checks.  The user needs to own the parent table and all
-- the merging partitions to do the merge.
CREATE ROLE regress_partition_merge_alice;
CREATE ROLE regress_partition_merge_bob;
GRANT ALL ON SCHEMA partitions_merge_schema TO regress_partition_merge_alice;
GRANT ALL ON SCHEMA partitions_merge_schema TO regress_partition_merge_bob;

SET SESSION AUTHORIZATION regress_partition_merge_alice;
CREATE TABLE t (i int) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);

SET SESSION AUTHORIZATION regress_partition_merge_bob;
-- ERROR:  must be owner of table t
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
RESET SESSION AUTHORIZATION;

ALTER TABLE t OWNER TO regress_partition_merge_bob;
SET SESSION AUTHORIZATION regress_partition_merge_bob;
-- ERROR:  must be owner of table tp_0_1
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
RESET SESSION AUTHORIZATION;

ALTER TABLE tp_0_1 OWNER TO regress_partition_merge_bob;
SET SESSION AUTHORIZATION regress_partition_merge_bob;
-- ERROR:  must be owner of table tp_1_2
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
RESET SESSION AUTHORIZATION;

ALTER TABLE tp_1_2 OWNER TO regress_partition_merge_bob;
SET SESSION AUTHORIZATION regress_partition_merge_bob;
-- Ok:
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;
RESET SESSION AUTHORIZATION;

DROP TABLE t;

-- Test: we can't merge partitions with different owners
CREATE TABLE tp_0_1(i int);
ALTER TABLE tp_0_1 OWNER TO regress_partition_merge_alice;
CREATE TABLE tp_1_2(i int);
ALTER TABLE tp_1_2 OWNER TO regress_partition_merge_bob;

CREATE TABLE t (i int) PARTITION BY RANGE (i);

ALTER TABLE t ATTACH PARTITION tp_0_1 FOR VALUES FROM (0) TO (1);
ALTER TABLE t ATTACH PARTITION tp_1_2 FOR VALUES FROM (1) TO (2);

-- Owner is 'regress_partition_merge_alice':
\dt tp_0_1
-- Owner is 'regress_partition_merge_bob':
\dt tp_1_2

-- ERROR:  partitions being merged have different owners
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;

DROP TABLE t;
REVOKE ALL ON SCHEMA partitions_merge_schema FROM regress_partition_merge_alice;
REVOKE ALL ON SCHEMA partitions_merge_schema FROM regress_partition_merge_bob;
DROP ROLE regress_partition_merge_alice;
DROP ROLE regress_partition_merge_bob;


-- Test for hash partitioned table
CREATE TABLE t (i int) PARTITION BY HASH(i);
CREATE TABLE tp1 PARTITION OF t FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE tp2 PARTITION OF t FOR VALUES WITH (MODULUS 2, REMAINDER 1);

-- ERROR:  partition of hash-partitioned table cannot be merged
ALTER TABLE t MERGE PARTITIONS (tp1, tp2) INTO tp3;

-- ERROR:  list of partitions to be merged should include at least two partitions
ALTER TABLE t MERGE PARTITIONS (tp1) INTO tp3;

DROP TABLE t;


-- Test for merged partition properties:
-- * STATISTICS is empty
-- * COMMENT is empty
-- * DEFAULTS are the same as DEFAULTS for partitioned table
-- * STORAGE is the same as STORAGE for partitioned table
-- * GENERATED and CONSTRAINTS are the same as GENERATED and CONSTRAINTS for partitioned table
-- * TRIGGERS are the same as TRIGGERS for partitioned table
\set HIDE_TOAST_COMPRESSION false

CREATE TABLE t
(i int NOT NULL,
 t text STORAGE EXTENDED COMPRESSION pglz DEFAULT 'default_t',
 b bigint,
 d date GENERATED ALWAYS as ('2022-01-01') STORED) PARTITION BY RANGE (abs(i));
COMMENT ON COLUMN t.i IS 't1.i';

CREATE TABLE tp_0_1
(i int NOT NULL,
 t text STORAGE MAIN DEFAULT 'default_tp_0_1',
 b bigint,
 d date GENERATED ALWAYS as ('2022-02-02') STORED);
ALTER TABLE t ATTACH PARTITION tp_0_1 FOR VALUES FROM (0) TO (1);
COMMENT ON COLUMN tp_0_1.i IS 'tp_0_1.i';

CREATE TABLE tp_1_2
(i int NOT NULL,
 t text STORAGE MAIN DEFAULT 'default_tp_1_2',
 b bigint,
 d date GENERATED ALWAYS as ('2022-03-03') STORED);
ALTER TABLE t ATTACH PARTITION tp_1_2 FOR VALUES FROM (1) TO (2);
COMMENT ON COLUMN tp_1_2.i IS 'tp_1_2.i';

CREATE STATISTICS t_stat (DEPENDENCIES) on i, b from t;
CREATE STATISTICS tp_0_1_stat (DEPENDENCIES) on i, b from tp_0_1;
CREATE STATISTICS tp_1_2_stat (DEPENDENCIES) on i, b from tp_1_2;

ALTER TABLE t ADD CONSTRAINT t_b_check CHECK (b > 0);
ALTER TABLE t ADD CONSTRAINT t_b_check1 CHECK (b > 0) NOT ENFORCED;
ALTER TABLE t ADD CONSTRAINT t_b_check2 CHECK (b > 0) NOT VALID;
ALTER TABLE t ADD CONSTRAINT t_b_nn NOT NULL b NOT VALID;

INSERT INTO tp_0_1(i, t, b) VALUES(0, DEFAULT, 1);
INSERT INTO tp_1_2(i, t, b) VALUES(1, DEFAULT, 2);
CREATE OR REPLACE FUNCTION trigger_function() RETURNS trigger LANGUAGE 'plpgsql' AS
$BODY$
BEGIN
  RAISE NOTICE 'trigger(%) called: action = %, when = %, level = %', TG_ARGV[0], TG_OP, TG_WHEN, TG_LEVEL;
  RETURN new;
END;
$BODY$;

CREATE TRIGGER t_before_insert_row_trigger BEFORE INSERT ON t FOR EACH ROW
  EXECUTE PROCEDURE trigger_function('t');
CREATE TRIGGER tp_0_1_before_insert_row_trigger BEFORE INSERT ON tp_0_1 FOR EACH ROW
  EXECUTE PROCEDURE trigger_function('tp_0_1');
CREATE TRIGGER tp_1_2_before_insert_row_trigger BEFORE INSERT ON tp_1_2 FOR EACH ROW
  EXECUTE PROCEDURE trigger_function('tp_1_2');

\d+ tp_0_1
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_1;
\d+ tp_0_1

INSERT INTO t(i, t, b) VALUES(1, DEFAULT, 3);
SELECT tableoid::regclass, * FROM t ORDER BY b;
DROP TABLE t;
DROP FUNCTION trigger_function();
\set HIDE_TOAST_COMPRESSION true


-- Test MERGE PARTITIONS with not valid foreign key constraint
CREATE TABLE t (i INT PRIMARY KEY) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
INSERT INTO t VALUES (0), (1);
CREATE TABLE t_fk (i INT);
INSERT INTO t_fk VALUES (1), (2);
ALTER TABLE t_fk ADD CONSTRAINT t_fk_i_fkey FOREIGN KEY (i) REFERENCES t NOT VALID;
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;

-- Should be NOT VALID FOREIGN KEY
\d tp_0_2
-- ERROR:  insert or update on table "t_fk" violates foreign key constraint "t_fk_i_fkey"
ALTER TABLE t_fk VALIDATE CONSTRAINT t_fk_i_fkey;

DROP TABLE t_fk;
DROP TABLE t;

-- Test MERGE PARTITIONS with not enforced foreign key constraint
CREATE TABLE t (i INT PRIMARY KEY) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
INSERT INTO t VALUES (0), (1);
CREATE TABLE t_fk (i INT);
INSERT INTO t_fk VALUES (1), (2);

ALTER TABLE t_fk ADD CONSTRAINT t_fk_i_fkey FOREIGN KEY (i) REFERENCES t NOT ENFORCED;
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;

-- Should be NOT ENFORCED FOREIGN KEY
\d tp_0_2
-- ERROR:  insert or update on table "t_fk" violates foreign key constraint "t_fk_i_fkey"
ALTER TABLE t_fk ALTER CONSTRAINT t_fk_i_fkey ENFORCED;

DROP TABLE t_fk;
DROP TABLE t;


-- Test for recomputation of stored generated columns.
CREATE TABLE t (i int, tab_id int generated always as (tableoid) stored) PARTITION BY RANGE (i);
CREATE TABLE tp_0_1 PARTITION OF t FOR VALUES FROM (0) TO (1);
CREATE TABLE tp_1_2 PARTITION OF t FOR VALUES FROM (1) TO (2);
ALTER TABLE t ADD CONSTRAINT cc CHECK(tableoid <> 123456789);
INSERT INTO t VALUES (0), (1);

-- Should be 0 because partition identifier for row with i=0 is different from
-- partition identifier for row with i=1.
SELECT count(*) FROM t WHERE i = 0 AND tab_id IN (SELECT tab_id FROM t WHERE i = 1);

-- "tab_id" column (stored generated column) with "tableoid" attribute requires
-- recomputation here.
ALTER TABLE t MERGE PARTITIONS (tp_0_1, tp_1_2) INTO tp_0_2;

-- Should be 1 because partition identifier for row with i=0 is the same as
-- partition identifier for row with i=1.
SELECT count(*) FROM t WHERE i = 0 AND tab_id IN (SELECT tab_id FROM t WHERE i = 1);

DROP TABLE t;


-- Test for generated columns (different order of columns in partitioned table
-- and partitions).
CREATE TABLE t (i int, g int GENERATED ALWAYS AS (i + tableoid::int)) PARTITION BY RANGE (i);
CREATE TABLE tp_1 (g int GENERATED ALWAYS AS (i + tableoid::int), i int);
CREATE TABLE tp_2 (g int GENERATED ALWAYS AS (i + tableoid::int), i int);
ALTER TABLE t ATTACH PARTITION tp_1 FOR VALUES FROM (-1) TO (10);
ALTER TABLE t ATTACH PARTITION tp_2 FOR VALUES FROM (10) TO (20);
ALTER TABLE t ADD CHECK (g > 0);
ALTER TABLE t ADD CHECK (i > 0);
INSERT INTO t VALUES (5), (15);

ALTER TABLE t MERGE PARTITIONS (tp_1, tp_2) INTO tp_12;

INSERT INTO t VALUES (16);
-- ERROR:  new row for relation "tp_12" violates check constraint "t_i_check"
INSERT INTO t VALUES (0);
-- Should be 3 rows: (5), (15), (16):
SELECT i FROM t ORDER BY i;
-- Should be 1 because for the same tableoid (15 + tableoid) = (5 + tableoid) + 10:
SELECT count(*) FROM t WHERE i = 15 AND g IN (SELECT g + 10 FROM t WHERE i = 5);

DROP TABLE t;


RESET search_path;

--
DROP SCHEMA partitions_merge_schema;
DROP SCHEMA partitions_merge_schema2;
