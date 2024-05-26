--
-- PARTITION_SPLIT
-- Tests for "ALTER TABLE ... SPLIT PARTITION ..." command
--

CREATE SCHEMA partition_split_schema;
CREATE SCHEMA partition_split_schema2;
SET search_path = partition_split_schema, public;

--
-- BY RANGE partitioning
--

--
-- Test for error codes
--
CREATE TABLE sales_range (salesperson_id int, salesperson_name varchar(30), sales_amount int, sales_date date) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb_mar_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

-- ERROR:  relation "sales_xxx" does not exist
ALTER TABLE sales_range SPLIT PARTITION sales_xxx INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  relation "sales_jan2022" already exists
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_jan2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  invalid bound specification for a range partition
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_jan2022 FOR VALUES IN ('2022-05-01', '2022-06-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  empty range bound specified for partition "sales_mar2022"
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-02-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

--ERROR:  list of split partitions should contain at least two items
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-10-01'));

-- ERROR:  lower bound of partition "sales_feb2022" is less than lower bound of split partition
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-01-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  name "sales_feb_mar_apr2022" is already used
-- (We can create partition with the same name as split partition, but can't create two partitions with the same name)
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb_mar_apr2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_feb_mar_apr2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  name "sales_feb2022" is already used
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_feb2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  "sales_feb_mar_apr2022" is not a partitioned table
ALTER TABLE sales_feb_mar_apr2022 SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_jan2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_feb2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- ERROR:  upper bound of partition "sales_apr2022" is greater than upper bound of split partition
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-06-01'));

-- ERROR:  lower bound of partition "sales_mar2022" conflicts with upper bound of previous partition "sales_feb2022"
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-02-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- Tests for spaces between partitions, them should be executed without DEFAULT partition
ALTER TABLE sales_range DETACH PARTITION sales_others;

-- ERROR:  lower bound of partition "sales_feb2022" is not equal to lower bound of split partition
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-02') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- Check the source partition not in the search path
SET search_path = partition_split_schema2, public;
ALTER TABLE partition_split_schema.sales_range
SPLIT PARTITION partition_split_schema.sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));
SET search_path = partition_split_schema, public;
\d+ sales_range

DROP TABLE sales_range;
DROP TABLE sales_others;

--
-- Add rows into partitioned table then split partition
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb_mar_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-10');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-11');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

SELECT * FROM sales_range;
SELECT * FROM sales_jan2022;
SELECT * FROM sales_feb2022;
SELECT * FROM sales_mar2022;
SELECT * FROM sales_apr2022;
SELECT * FROM sales_others;

DROP TABLE sales_range CASCADE;

--
-- Add split partition, then add rows into partitioned table
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb_mar_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

-- Split partition, also check schema qualification of new partitions
ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION partition_split_schema.sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION partition_split_schema2.sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));
\d+ sales_range

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-10');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-11');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

SELECT * FROM sales_range;
SELECT * FROM sales_jan2022;
SELECT * FROM sales_feb2022;
SELECT * FROM partition_split_schema2.sales_mar2022;
SELECT * FROM sales_apr2022;
SELECT * FROM sales_others;

DROP TABLE sales_range CASCADE;

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
CREATE TABLE sales_jan_feb2022 PARTITION OF sales_date FOR VALUES FROM (2022, 1, 1) TO (2022, 3, 1);
CREATE TABLE sales_other PARTITION OF sales_date FOR VALUES FROM (2022, 3, 1) TO (MAXVALUE, MAXVALUE, MAXVALUE);

INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager1', 2021, 12, 7);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager2', 2021, 12, 8);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager3', 2022, 1, 1);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager1', 2022, 2, 4);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager2', 2022, 1, 2);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager3', 2022, 2, 1);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager1', 2022, 3, 3);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager2', 2022, 3, 4);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager3', 2022, 5, 1);

SELECT * FROM sales_date;
SELECT * FROM sales_dec2022;
SELECT * FROM sales_jan_feb2022;
SELECT * FROM sales_other;

ALTER TABLE sales_date SPLIT PARTITION sales_jan_feb2022 INTO
  (PARTITION sales_jan2022 FOR VALUES FROM (2022, 1, 1) TO (2022, 2, 1),
   PARTITION sales_feb2022 FOR VALUES FROM (2022, 2, 1) TO (2022, 3, 1));

INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager1', 2022, 1, 10);
INSERT INTO sales_date(salesperson_name, sales_year, sales_month, sales_day) VALUES ('Manager2', 2022, 2, 10);

SELECT * FROM sales_date;
SELECT * FROM sales_dec2022;
SELECT * FROM sales_jan2022;
SELECT * FROM sales_feb2022;
SELECT * FROM sales_other;

--ERROR:  relation "sales_jan_feb2022" does not exist
SELECT * FROM sales_jan_feb2022;

DROP TABLE sales_date CASCADE;

--
-- Test: split DEFAULT partition; use an index on partition key; check index after split
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;
CREATE INDEX sales_range_sales_date_idx ON sales_range USING btree (sales_date);

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-10');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-11');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

SELECT * FROM sales_others;
SELECT * FROM pg_indexes WHERE tablename = 'sales_others' and schemaname = 'partition_split_schema' ORDER BY indexname;

ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'),
   PARTITION sales_others DEFAULT);

-- Use indexscan for testing indexes
SET enable_indexscan = ON;
SET enable_seqscan = OFF;

SELECT * FROM sales_feb2022 where sales_date > '2022-01-01';
SELECT * FROM sales_mar2022 where sales_date > '2022-01-01';
SELECT * FROM sales_apr2022 where sales_date > '2022-01-01';
SELECT * FROM sales_others where sales_date > '2022-01-01';

SET enable_indexscan = ON;
SET enable_seqscan = ON;

SELECT * FROM pg_indexes WHERE tablename = 'sales_feb2022' and schemaname = 'partition_split_schema' ORDER BY indexname;
SELECT * FROM pg_indexes WHERE tablename = 'sales_mar2022' and schemaname = 'partition_split_schema' ORDER BY indexname;
SELECT * FROM pg_indexes WHERE tablename = 'sales_apr2022' and schemaname = 'partition_split_schema' ORDER BY indexname;
SELECT * FROM pg_indexes WHERE tablename = 'sales_others' and schemaname = 'partition_split_schema' ORDER BY indexname;

DROP TABLE sales_range CASCADE;

--
-- Test: some cases for splitting DEFAULT partition (different bounds)
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date INT) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

-- sales_error intersects with sales_dec2022 (lower bound)
-- ERROR:  lower bound of partition "sales_error" conflicts with upper bound of previous partition "sales_dec2022"
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_error FOR VALUES FROM (20211230) TO (20220201),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

-- sales_error intersects with sales_feb2022 (upper bound)
-- ERROR:  lower bound of partition "sales_feb2022" conflicts with upper bound of previous partition "sales_error"
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_error FOR VALUES FROM (20220101) TO (20220202),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

-- sales_error intersects with sales_dec2022 (inside bound)
-- ERROR:  lower bound of partition "sales_error" conflicts with upper bound of previous partition "sales_dec2022"
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_error FOR VALUES FROM (20211210) TO (20211220),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

-- sales_error intersects with sales_dec2022 (exactly the same bounds)
-- ERROR:  lower bound of partition "sales_error" conflicts with upper bound of previous partition "sales_dec2022"
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_error FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

-- ERROR:  all partitions in the list should be DEFAULT because split partition is DEFAULT
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_jan2022 FOR VALUES FROM (20220101) TO (20220201),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301));

-- no error: bounds of sales_noerror are between sales_dec2022 and sales_feb2022
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_noerror FOR VALUES FROM (20220110) TO (20220120),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

DROP TABLE sales_range;

CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date INT) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

-- no error: bounds of sales_noerror are equal to lower and upper bounds of sales_dec2022 and sales_feb2022
ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_dec2022 FOR VALUES FROM (20211201) TO (20220101),
   PARTITION sales_noerror FOR VALUES FROM (20210101) TO (20210201),
   PARTITION sales_feb2022 FOR VALUES FROM (20220201) TO (20220301),
   PARTITION sales_others DEFAULT);

DROP TABLE sales_range;

--
-- Test: split partition with CHECK and FOREIGN KEY CONSTRAINTs on partitioned table
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30));
INSERT INTO salespeople VALUES (1,  'Poirot');

CREATE TABLE sales_range (
salesperson_id INT REFERENCES salespeople(salesperson_id),
sales_amount INT CHECK (sales_amount > 1),
sales_date DATE) PARTITION BY RANGE (sales_date);

CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb_mar_apr2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'sales_feb_mar_apr2022'::regclass::oid ORDER BY conname;

ALTER TABLE sales_range SPLIT PARTITION sales_feb_mar_apr2022 INTO
  (PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_mar2022 FOR VALUES FROM ('2022-03-01') TO ('2022-04-01'),
   PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'));

-- We should see the same CONSTRAINTs as on sales_feb_mar_apr2022 partition
SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'sales_feb2022'::regclass::oid ORDER BY conname;;
SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'sales_mar2022'::regclass::oid ORDER BY conname;;
SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'sales_apr2022'::regclass::oid ORDER BY conname;;

-- ERROR:  new row for relation "sales_mar2022" violates check constraint "sales_range_sales_amount_check"
INSERT INTO sales_range VALUES (1, 0, '2022-03-11');
-- ERROR:  insert or update on table "sales_mar2022" violates foreign key constraint "sales_range_salesperson_id_fkey"
INSERT INTO sales_range VALUES (-1, 10, '2022-03-11');
-- ok
INSERT INTO sales_range VALUES (1, 10, '2022-03-11');

DROP TABLE sales_range CASCADE;
DROP TABLE salespeople CASCADE;

--
-- Test: split partition on partitioned table in case of existing FOREIGN KEY reference from another table
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);
CREATE TABLE sales (salesperson_id INT REFERENCES salespeople(salesperson_id), sales_amount INT, sales_date DATE);

CREATE TABLE salespeople01_10 PARTITION OF salespeople FOR VALUES FROM (1) TO (10);
CREATE TABLE salespeople10_40 PARTITION OF salespeople FOR VALUES FROM (10) TO (40);

INSERT INTO salespeople VALUES (1,  'Poirot');
INSERT INTO salespeople VALUES (10, 'May');
INSERT INTO salespeople VALUES (19, 'Ivanov');
INSERT INTO salespeople VALUES (20, 'Smirnoff');
INSERT INTO salespeople VALUES (30, 'Ford');

INSERT INTO sales VALUES (1,  100, '2022-03-01');
INSERT INTO sales VALUES (1,  110, '2022-03-02');
INSERT INTO sales VALUES (10, 150, '2022-03-01');
INSERT INTO sales VALUES (10, 90,  '2022-03-03');
INSERT INTO sales VALUES (19, 200, '2022-03-04');
INSERT INTO sales VALUES (20, 50,  '2022-03-12');
INSERT INTO sales VALUES (20, 170, '2022-03-02');
INSERT INTO sales VALUES (30, 30,  '2022-03-04');

SELECT * FROM salespeople01_10;
SELECT * FROM salespeople10_40;

ALTER TABLE salespeople SPLIT PARTITION salespeople10_40 INTO
  (PARTITION salespeople10_20 FOR VALUES FROM (10) TO (20),
   PARTITION salespeople20_30 FOR VALUES FROM (20) TO (30),
   PARTITION salespeople30_40 FOR VALUES FROM (30) TO (40));

SELECT * FROM salespeople01_10;
SELECT * FROM salespeople10_20;
SELECT * FROM salespeople20_30;
SELECT * FROM salespeople30_40;

-- ERROR:  insert or update on table "sales" violates foreign key constraint "sales_salesperson_id_fkey"
INSERT INTO sales VALUES (40, 50,  '2022-03-04');
-- ok
INSERT INTO sales VALUES (30, 50,  '2022-03-04');

DROP TABLE sales CASCADE;
DROP TABLE salespeople CASCADE;

--
-- Test: split partition of partitioned table with triggers
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);

CREATE TABLE salespeople01_10 PARTITION OF salespeople FOR VALUES FROM (1) TO (10);
CREATE TABLE salespeople10_40 PARTITION OF salespeople FOR VALUES FROM (10) TO (40);

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
INSERT INTO salespeople10_40 VALUES (19, 'Ivanov');

ALTER TABLE salespeople SPLIT PARTITION salespeople10_40 INTO
  (PARTITION salespeople10_20 FOR VALUES FROM (10) TO (20),
   PARTITION salespeople20_30 FOR VALUES FROM (20) TO (30),
   PARTITION salespeople30_40 FOR VALUES FROM (30) TO (40));

-- 2 triggers should fire here (row + statement):
INSERT INTO salespeople VALUES (20, 'Smirnoff');
-- 1 trigger should fire here (row):
INSERT INTO salespeople30_40 VALUES (30, 'Ford');

SELECT * FROM salespeople01_10;
SELECT * FROM salespeople10_20;
SELECT * FROM salespeople20_30;
SELECT * FROM salespeople30_40;

DROP TABLE salespeople CASCADE;
DROP FUNCTION after_insert_row_trigger();

--
-- Test: split partition witch identity column
-- If split partition column is identity column, columns of new partitions are identity columns too.
--
CREATE TABLE salespeople(salesperson_id INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);

CREATE TABLE salespeople1_2 PARTITION OF salespeople FOR VALUES FROM (1) TO (2);
-- Create new partition with identity column:
CREATE TABLE salespeople2_5(salesperson_id INT NOT NULL, salesperson_name VARCHAR(30));
ALTER TABLE salespeople ATTACH PARTITION salespeople2_5 FOR VALUES FROM (2) TO (5);

INSERT INTO salespeople (salesperson_name) VALUES ('Poirot');
INSERT INTO salespeople (salesperson_name) VALUES ('Ivanov');

SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople'::regclass::oid ORDER BY attnum;
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople1_2'::regclass::oid ORDER BY attnum;
-- Split partition has identity column:
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople2_5'::regclass::oid ORDER BY attnum;

ALTER TABLE salespeople SPLIT PARTITION salespeople2_5 INTO
  (PARTITION salespeople2_3 FOR VALUES FROM (2) TO (3),
   PARTITION salespeople3_4 FOR VALUES FROM (3) TO (4),
   PARTITION salespeople4_5 FOR VALUES FROM (4) TO (5));

INSERT INTO salespeople (salesperson_name) VALUES ('May');
INSERT INTO salespeople (salesperson_name) VALUES ('Ford');

SELECT * FROM salespeople1_2;
SELECT * FROM salespeople2_3;
SELECT * FROM salespeople3_4;
SELECT * FROM salespeople4_5;

SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople'::regclass::oid ORDER BY attnum;
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople1_2'::regclass::oid ORDER BY attnum;
-- New partitions have identity-columns:
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople2_3'::regclass::oid ORDER BY attnum;
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople3_4'::regclass::oid ORDER BY attnum;
SELECT attname, attidentity, attgenerated FROM pg_attribute WHERE attnum > 0 AND attrelid = 'salespeople4_5'::regclass::oid ORDER BY attnum;

DROP TABLE salespeople CASCADE;

--
-- Test: split partition with deleted columns
--
CREATE TABLE salespeople(salesperson_id INT PRIMARY KEY, salesperson_name VARCHAR(30)) PARTITION BY RANGE (salesperson_id);

CREATE TABLE salespeople01_10 PARTITION OF salespeople FOR VALUES FROM (1) TO (10);
-- Create new partition with some deleted columns:
CREATE TABLE salespeople10_40(d1 VARCHAR(30), salesperson_id INT PRIMARY KEY, d2 INT, d3 DATE, salesperson_name VARCHAR(30));

INSERT INTO salespeople10_40 VALUES ('dummy value 1', 19, 100, now(), 'Ivanov');
INSERT INTO salespeople10_40 VALUES ('dummy value 2', 20, 101, now(), 'Smirnoff');

ALTER TABLE salespeople10_40 DROP COLUMN d1;
ALTER TABLE salespeople10_40 DROP COLUMN d2;
ALTER TABLE salespeople10_40 DROP COLUMN d3;

ALTER TABLE salespeople ATTACH PARTITION salespeople10_40 FOR VALUES FROM (10) TO (40);

INSERT INTO salespeople VALUES (1, 'Poirot');
INSERT INTO salespeople VALUES (10, 'May');
INSERT INTO salespeople VALUES (30, 'Ford');

ALTER TABLE salespeople SPLIT PARTITION salespeople10_40 INTO
  (PARTITION salespeople10_20 FOR VALUES FROM (10) TO (20),
   PARTITION salespeople20_30 FOR VALUES FROM (20) TO (30),
   PARTITION salespeople30_40 FOR VALUES FROM (30) TO (40));

select * from salespeople01_10;
select * from salespeople10_20;
select * from salespeople20_30;
select * from salespeople30_40;

DROP TABLE salespeople CASCADE;

--
-- Test: split sub-partition
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_feb2022 PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE sales_mar2022 PARTITION OF sales_range FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');

CREATE TABLE sales_apr2022 (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_apr_all PARTITION OF sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');
ALTER TABLE sales_range ATTACH PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01');

CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

CREATE INDEX sales_range_sales_date_idx ON sales_range USING btree (sales_date);

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-10');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-11');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

SELECT * FROM sales_range;
SELECT * FROM sales_apr2022;

ALTER TABLE sales_apr2022 SPLIT PARTITION sales_apr_all INTO
  (PARTITION sales_apr2022_01_10 FOR VALUES FROM ('2022-04-01') TO ('2022-04-10'),
   PARTITION sales_apr2022_10_20 FOR VALUES FROM ('2022-04-10') TO ('2022-04-20'),
   PARTITION sales_apr2022_20_30 FOR VALUES FROM ('2022-04-20') TO ('2022-05-01'));

SELECT * FROM sales_range;
SELECT * FROM sales_apr2022;
SELECT * FROM sales_apr2022_01_10;
SELECT * FROM sales_apr2022_10_20;
SELECT * FROM sales_apr2022_20_30;

DROP TABLE sales_range;

--
-- BY LIST partitioning
--

--
-- Test: specific errors for BY LIST partitioning
--
CREATE TABLE sales_list
(salesperson_id INT,
 salesperson_name VARCHAR(30),
 sales_state VARCHAR(20),
 sales_amount INT,
 sales_date DATE)
PARTITION BY LIST (sales_state);

CREATE TABLE sales_nord PARTITION OF sales_list FOR VALUES IN ('Oslo', 'St. Petersburg', 'Helsinki');
CREATE TABLE sales_all PARTITION OF sales_list FOR VALUES IN ('Warsaw', 'Lisbon', 'New York', 'Madrid', 'Bejing', 'Berlin', 'Delhi', 'Kyiv', 'Vladivostok');
CREATE TABLE sales_others PARTITION OF sales_list DEFAULT;

-- ERROR:  new partition "sales_east" would overlap with another (not split) partition "sales_nord"
ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid'),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok', 'Helsinki'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv'));

-- ERROR:  new partition "sales_west" would overlap with another new partition "sales_central"
ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid'),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', 'Lisbon', 'Kyiv'));

-- ERROR:  new partition "sales_west" cannot have NULL value because split partition does not have
ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid', NULL),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv'));

DROP TABLE sales_list;

--
-- Test: two specific errors for BY LIST partitioning:
--   * new partitions do not have NULL value, which split partition has.
--   * new partitions do not have a value that split partition has.
--
CREATE TABLE sales_list
(salesperson_id INT,
 salesperson_name VARCHAR(30),
 sales_state VARCHAR(20),
 sales_amount INT,
 sales_date DATE)
PARTITION BY LIST (sales_state);

CREATE TABLE sales_nord PARTITION OF sales_list FOR VALUES IN ('Helsinki', 'St. Petersburg', 'Oslo');
CREATE TABLE sales_all PARTITION OF sales_list FOR VALUES IN ('Warsaw', 'Lisbon', 'New York', 'Madrid', 'Bejing', 'Berlin', 'Delhi', 'Kyiv', 'Vladivostok', NULL);

-- ERROR:  new partitions do not have value NULL but split partition does
ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid'),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv'));

-- ERROR:  new partitions do not have value 'Kyiv' but split partition does
ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid'),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', NULL));

DROP TABLE sales_list;

--
-- Test: BY LIST partitioning, SPLIT PARTITION with data
--
CREATE TABLE sales_list
(salesperson_id SERIAL,
 salesperson_name VARCHAR(30),
 sales_state VARCHAR(20),
 sales_amount INT,
 sales_date DATE)
PARTITION BY LIST (sales_state);

CREATE INDEX sales_list_salesperson_name_idx ON sales_list USING btree (salesperson_name);
CREATE INDEX sales_list_sales_state_idx ON sales_list USING btree (sales_state);

CREATE TABLE sales_nord PARTITION OF sales_list FOR VALUES IN ('Helsinki', 'St. Petersburg', 'Oslo');
CREATE TABLE sales_all PARTITION OF sales_list FOR VALUES IN ('Warsaw', 'Lisbon', 'New York', 'Madrid', 'Bejing', 'Berlin', 'Delhi', 'Kyiv', 'Vladivostok');
CREATE TABLE sales_others PARTITION OF sales_list DEFAULT;

INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Trump',    'Bejing',         1000, '2022-03-01');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Smirnoff', 'New York',        500, '2022-03-03');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Ford',     'St. Petersburg', 2000, '2022-03-05');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Ivanov',   'Warsaw',          750, '2022-03-04');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Deev',     'Lisbon',          250, '2022-03-07');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Poirot',   'Berlin',         1000, '2022-03-01');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('May',      'Oslo',           1200, '2022-03-06');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Li',       'Vladivostok',    1150, '2022-03-09');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('May',      'Oslo',           1200, '2022-03-11');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Halder',   'Helsinki',        800, '2022-03-02');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Muller',   'Madrid',          650, '2022-03-05');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Smith',    'Kyiv',            350, '2022-03-10');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Gandi',    'Warsaw',          150, '2022-03-08');
INSERT INTO sales_list (salesperson_name, sales_state, sales_amount, sales_date) VALUES ('Plato',    'Lisbon',          950, '2022-03-05');

ALTER TABLE sales_list SPLIT PARTITION sales_all INTO
  (PARTITION sales_west FOR VALUES IN ('Lisbon', 'New York', 'Madrid'),
   PARTITION sales_east FOR VALUES IN ('Bejing', 'Delhi', 'Vladivostok'),
   PARTITION sales_central FOR VALUES IN ('Warsaw', 'Berlin', 'Kyiv'));

SELECT * FROM sales_list;
SELECT * FROM sales_west;
SELECT * FROM sales_east;
SELECT * FROM sales_nord;
SELECT * FROM sales_central;

-- Use indexscan for testing indexes after splitting partition
SET enable_indexscan = ON;
SET enable_seqscan = OFF;

SELECT * FROM sales_central WHERE sales_state = 'Warsaw';
SELECT * FROM sales_list WHERE sales_state = 'Warsaw';
SELECT * FROM sales_list WHERE salesperson_name = 'Ivanov';

SET enable_indexscan = ON;
SET enable_seqscan = ON;

DROP TABLE sales_list;

--
-- Test for:
--   * split DEFAULT partition to partitions with spaces between bounds;
--   * random order of partitions in SPLIT PARTITION command.
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-09');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-07');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

ALTER TABLE sales_range SPLIT PARTITION sales_others INTO
  (PARTITION sales_others DEFAULT,
   PARTITION sales_mar2022_1decade FOR VALUES FROM ('2022-03-01') TO ('2022-03-10'),
   PARTITION sales_jan2022_1decade FOR VALUES FROM ('2022-01-01') TO ('2022-01-10'),
   PARTITION sales_feb2022_1decade FOR VALUES FROM ('2022-02-01') TO ('2022-02-10'),
   PARTITION sales_apr2022_1decade FOR VALUES FROM ('2022-04-01') TO ('2022-04-10'));

SELECT * FROM sales_jan2022_1decade;
SELECT * FROM sales_feb2022_1decade;
SELECT * FROM sales_mar2022_1decade;
SELECT * FROM sales_apr2022_1decade;
SELECT * FROM sales_others;

DROP TABLE sales_range;

--
-- Test for:
--   * split non-DEFAULT partition to partitions with spaces between bounds;
--   * random order of partitions in SPLIT PARTITION command.
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_all PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-05-01');
CREATE TABLE sales_others PARTITION OF sales_range DEFAULT;

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-09');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-07');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');
INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

ALTER TABLE sales_range SPLIT PARTITION sales_all INTO
  (PARTITION sales_mar2022_1decade FOR VALUES FROM ('2022-03-01') TO ('2022-03-10'),
   PARTITION sales_jan2022_1decade FOR VALUES FROM ('2022-01-01') TO ('2022-01-10'),
   PARTITION sales_feb2022_1decade FOR VALUES FROM ('2022-02-01') TO ('2022-02-10'),
   PARTITION sales_apr2022_1decade FOR VALUES FROM ('2022-04-01') TO ('2022-04-10'));

SELECT * FROM sales_jan2022_1decade;
SELECT * FROM sales_feb2022_1decade;
SELECT * FROM sales_mar2022_1decade;
SELECT * FROM sales_apr2022_1decade;
SELECT * FROM sales_others;

DROP TABLE sales_range;

--
-- Test for split non-DEFAULT partition to DEFAULT partition + partitions
-- with spaces between bounds.
--
CREATE TABLE sales_range (salesperson_id INT, salesperson_name VARCHAR(30), sales_amount INT, sales_date DATE) PARTITION BY RANGE (sales_date);
CREATE TABLE sales_jan2022 PARTITION OF sales_range FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE sales_all PARTITION OF sales_range FOR VALUES FROM ('2022-02-01') TO ('2022-05-01');

INSERT INTO sales_range VALUES (1,  'May',      1000, '2022-01-31');
INSERT INTO sales_range VALUES (2,  'Smirnoff', 500,  '2022-02-10');
INSERT INTO sales_range VALUES (3,  'Ford',     2000, '2022-04-30');
INSERT INTO sales_range VALUES (4,  'Ivanov',   750,  '2022-04-13');
INSERT INTO sales_range VALUES (5,  'Deev',     250,  '2022-04-07');
INSERT INTO sales_range VALUES (6,  'Poirot',   150,  '2022-02-11');
INSERT INTO sales_range VALUES (7,  'Li',       175,  '2022-03-08');
INSERT INTO sales_range VALUES (8,  'Ericsson', 185,  '2022-02-23');
INSERT INTO sales_range VALUES (9,  'Muller',   250,  '2022-03-11');
INSERT INTO sales_range VALUES (10, 'Halder',   350,  '2022-01-28');
INSERT INTO sales_range VALUES (11, 'Trump',    380,  '2022-04-06');
INSERT INTO sales_range VALUES (12, 'Plato',    350,  '2022-03-19');
INSERT INTO sales_range VALUES (13, 'Gandi',    377,  '2022-01-09');

ALTER TABLE sales_range SPLIT PARTITION sales_all INTO
  (PARTITION sales_apr2022 FOR VALUES FROM ('2022-04-01') TO ('2022-05-01'),
   PARTITION sales_feb2022 FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'),
   PARTITION sales_others DEFAULT);

INSERT INTO sales_range VALUES (14, 'Smith',    510,  '2022-05-04');

SELECT * FROM sales_range;
SELECT * FROM sales_jan2022;
SELECT * FROM sales_feb2022;
SELECT * FROM sales_apr2022;
SELECT * FROM sales_others;

DROP TABLE sales_range;

--
-- Try to SPLIT partition of another table.
--
CREATE TABLE t1(i int, t text) PARTITION BY LIST (t);
CREATE TABLE t1pa PARTITION OF t1 FOR VALUES IN ('A');
CREATE TABLE t2 (i int, t text) PARTITION BY RANGE (t);

-- ERROR:  relation "t1pa" is not a partition of relation "t2"
ALTER TABLE t2 SPLIT PARTITION t1pa INTO
   (PARTITION t2a FOR VALUES FROM ('A') TO ('B'),
    PARTITION t2b FOR VALUES FROM ('B') TO ('C'));

DROP TABLE t2;
DROP TABLE t1;

--
-- Try to SPLIT partition of temporary table.
--
CREATE TEMP TABLE t (i int) PARTITION BY RANGE (i);
CREATE TEMP TABLE tp_0_2 PARTITION OF t FOR VALUES FROM (0) TO (2);

SELECT c.oid::pg_catalog.regclass, pg_catalog.pg_get_expr(c.relpartbound, c.oid), c.relpersistence
  FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i
  WHERE c.oid = i.inhrelid AND i.inhparent = 't'::regclass
  ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT', c.oid::pg_catalog.regclass::pg_catalog.text;

ALTER TABLE t SPLIT PARTITION tp_0_2 INTO
  (PARTITION tp_0_1 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1_2 FOR VALUES FROM (1) TO (2));

-- Partitions should be temporary.
SELECT c.oid::pg_catalog.regclass, pg_catalog.pg_get_expr(c.relpartbound, c.oid), c.relpersistence
  FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i
  WHERE c.oid = i.inhrelid AND i.inhparent = 't'::regclass
  ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT', c.oid::pg_catalog.regclass::pg_catalog.text;

DROP TABLE t;

-- Check new partitions inherits parent's table access method
CREATE ACCESS METHOD partition_split_heap TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE t (i int) PARTITION BY RANGE (i) USING partition_split_heap;
CREATE TABLE tp_0_2 PARTITION OF t FOR VALUES FROM (0) TO (2);
ALTER TABLE t SPLIT PARTITION tp_0_2 INTO
  (PARTITION tp_0_1 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1_2 FOR VALUES FROM (1) TO (2));
SELECT c.relname, a.amname
FROM pg_class c JOIN pg_am a ON c.relam = a.oid
WHERE c.oid IN ('t'::regclass, 'tp_0_1'::regclass, 'tp_1_2'::regclass)
ORDER BY c.relname;
DROP TABLE t;
DROP ACCESS METHOD partition_split_heap;

-- Test permission checks.  The user needs to own the parent table and the
-- the partition to split to do the split.
CREATE ROLE regress_partition_split_alice;
CREATE ROLE regress_partition_split_bob;
GRANT ALL ON SCHEMA partition_split_schema TO regress_partition_split_alice;
GRANT ALL ON SCHEMA partition_split_schema TO regress_partition_split_bob;

SET SESSION AUTHORIZATION regress_partition_split_alice;
CREATE TABLE t (i int) PARTITION BY RANGE (i);
CREATE TABLE tp_0_2 PARTITION OF t FOR VALUES FROM (0) TO (2);

SET SESSION AUTHORIZATION regress_partition_split_bob;
ALTER TABLE t SPLIT PARTITION tp_0_2 INTO
  (PARTITION tp_0_1 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1_2 FOR VALUES FROM (1) TO (2));
RESET SESSION AUTHORIZATION;

ALTER TABLE t OWNER TO regress_partition_split_bob;
SET SESSION AUTHORIZATION regress_partition_split_bob;
ALTER TABLE t SPLIT PARTITION tp_0_2 INTO
  (PARTITION tp_0_1 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1_2 FOR VALUES FROM (1) TO (2));
RESET SESSION AUTHORIZATION;

ALTER TABLE tp_0_2 OWNER TO regress_partition_split_bob;
SET SESSION AUTHORIZATION regress_partition_split_bob;
ALTER TABLE t SPLIT PARTITION tp_0_2 INTO
  (PARTITION tp_0_1 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1_2 FOR VALUES FROM (1) TO (2));
RESET SESSION AUTHORIZATION;

DROP TABLE t;
REVOKE ALL ON SCHEMA partition_split_schema FROM regress_partition_split_alice;
REVOKE ALL ON SCHEMA partition_split_schema FROM regress_partition_split_bob;
DROP ROLE regress_partition_split_alice;
DROP ROLE regress_partition_split_bob;

-- Split partition of a temporary table when one of the partitions after
-- split has the same name as the partition being split
CREATE TEMP TABLE t (a int) PARTITION BY RANGE (a);
CREATE TEMP TABLE tp_0 PARTITION OF t FOR VALUES FROM (0) TO (2);
ALTER TABLE t SPLIT PARTITION tp_0 INTO
  (PARTITION tp_0 FOR VALUES FROM (0) TO (1),
   PARTITION tp_1 FOR VALUES FROM (1) TO (2));
DROP TABLE t;

RESET search_path;

--
DROP SCHEMA partition_split_schema;
DROP SCHEMA partition_split_schema2;
