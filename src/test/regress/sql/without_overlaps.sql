-- Tests for WITHOUT OVERLAPS.
--
-- We leave behind several tables to test pg_dump etc:
-- temporal_rng, temporal_rng2,
-- temporal_fk_rng2rng.

SET datestyle TO ISO, YMD;

--
-- test input parser
--

-- PK with no columns just WITHOUT OVERLAPS:

CREATE TABLE temporal_rng (
	valid_at daterange,
	CONSTRAINT temporal_rng_pk PRIMARY KEY (valid_at WITHOUT OVERLAPS)
);

-- PK with a range column/PERIOD that isn't there:

CREATE TABLE temporal_rng (
	id INTEGER,
	CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);

-- PK with a non-range column:

CREATE TABLE temporal_rng (
	id int4range,
	valid_at TEXT,
	CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);

-- PK with one column plus a range:

CREATE TABLE temporal_rng (
	-- Since we can't depend on having btree_gist here,
	-- use an int4range instead of an int.
	-- (The rangetypes regression test uses the same trick.)
	id int4range,
	valid_at daterange,
	CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng_pk';

-- PK with two columns plus a range:
-- We don't drop this table because tests below also need multiple scalar columns.
CREATE TABLE temporal_rng2 (
	id1 int4range,
	id2 int4range,
	valid_at daterange,
	CONSTRAINT temporal_rng2_pk PRIMARY KEY (id1, id2, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng2
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng2_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng2_pk';


-- PK with a custom range type:
CREATE TYPE textrange2 AS range (subtype=text, collation="C");
CREATE TABLE temporal_rng3 (
	id int4range,
	valid_at textrange2,
	CONSTRAINT temporal_rng3_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
ALTER TABLE temporal_rng3 DROP CONSTRAINT temporal_rng3_pk;
DROP TABLE temporal_rng3;
DROP TYPE textrange2;

-- PK with a multirange:
CREATE TABLE temporal_mltrng (
  id int4range,
  valid_at tsmultirange,
  CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_mltrng

-- UNIQUE with no columns just WITHOUT OVERLAPS:

CREATE TABLE temporal_rng3 (
	valid_at daterange,
	CONSTRAINT temporal_rng3_uq UNIQUE (valid_at WITHOUT OVERLAPS)
);

-- UNIQUE with a range column/PERIOD that isn't there:

CREATE TABLE temporal_rng3 (
	id INTEGER,
	CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);

-- UNIQUE with a non-range column:

CREATE TABLE temporal_rng3 (
	id int4range,
	valid_at TEXT,
	CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);

-- UNIQUE with one column plus a range:

CREATE TABLE temporal_rng3 (
	id int4range,
	valid_at daterange,
	CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng3
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng3_uq';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng3_uq';
DROP TABLE temporal_rng3;

-- UNIQUE with two columns plus a range:
CREATE TABLE temporal_rng3 (
	id1 int4range,
	id2 int4range,
	valid_at daterange,
	CONSTRAINT temporal_rng3_uq UNIQUE (id1, id2, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng3
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng3_uq';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng3_uq';
DROP TABLE temporal_rng3;

-- UNIQUE with a custom range type:
CREATE TYPE textrange2 AS range (subtype=text, collation="C");
CREATE TABLE temporal_rng3 (
	id int4range,
	valid_at textrange2,
	CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);
ALTER TABLE temporal_rng3 DROP CONSTRAINT temporal_rng3_uq;
DROP TABLE temporal_rng3;
DROP TYPE textrange2;

--
-- test ALTER TABLE ADD CONSTRAINT
--

DROP TABLE temporal_rng;
CREATE TABLE temporal_rng (
	id int4range,
	valid_at daterange
);
ALTER TABLE temporal_rng
	ADD CONSTRAINT temporal_rng_pk
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);

-- PK with USING INDEX (not possible):
CREATE TABLE temporal3 (
	id int4range,
	valid_at daterange
);
CREATE INDEX idx_temporal3_uq ON temporal3 USING gist (id, valid_at);
ALTER TABLE temporal3
	ADD CONSTRAINT temporal3_pk
	PRIMARY KEY USING INDEX idx_temporal3_uq;
DROP TABLE temporal3;

-- UNIQUE with USING INDEX (not possible):
CREATE TABLE temporal3 (
	id int4range,
	valid_at daterange
);
CREATE INDEX idx_temporal3_uq ON temporal3 USING gist (id, valid_at);
ALTER TABLE temporal3
	ADD CONSTRAINT temporal3_uq
	UNIQUE USING INDEX idx_temporal3_uq;
DROP TABLE temporal3;

-- UNIQUE with USING [UNIQUE] INDEX (possible but not a temporal constraint):
CREATE TABLE temporal3 (
	id int4range,
	valid_at daterange
);
CREATE UNIQUE INDEX idx_temporal3_uq ON temporal3 (id, valid_at);
ALTER TABLE temporal3
	ADD CONSTRAINT temporal3_uq
	UNIQUE USING INDEX idx_temporal3_uq;
DROP TABLE temporal3;

-- Add range column and the PK at the same time
CREATE TABLE temporal3 (
	id int4range
);
ALTER TABLE temporal3
	ADD COLUMN valid_at daterange,
	ADD CONSTRAINT temporal3_pk
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
DROP TABLE temporal3;

-- Add range column and UNIQUE constraint at the same time
CREATE TABLE temporal3 (
	id int4range
);
ALTER TABLE temporal3
	ADD COLUMN valid_at daterange,
	ADD CONSTRAINT temporal3_uq
	UNIQUE (id, valid_at WITHOUT OVERLAPS);
DROP TABLE temporal3;

--
-- test PK inserts
--

-- okay:
INSERT INTO temporal_rng VALUES ('[1,1]', daterange('2018-01-02', '2018-02-03'));
INSERT INTO temporal_rng VALUES ('[1,1]', daterange('2018-03-03', '2018-04-04'));
INSERT INTO temporal_rng VALUES ('[2,2]', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng VALUES ('[3,3]', daterange('2018-01-01', NULL));

-- should fail:
INSERT INTO temporal_rng VALUES ('[1,1]', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng VALUES (NULL, daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng VALUES ('[3,3]', NULL);

--
-- test a range with both a PK and a UNIQUE constraint
--

CREATE TABLE temporal3 (
  id int4range,
  valid_at daterange,
  id2 int8range,
  name TEXT,
  CONSTRAINT temporal3_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal3_uniq UNIQUE (id2, valid_at WITHOUT OVERLAPS)
);
INSERT INTO temporal3 (id, valid_at, id2, name)
  VALUES
  ('[1,1]', daterange('2000-01-01', '2010-01-01'), '[7,7]', 'foo'),
  ('[2,2]', daterange('2000-01-01', '2010-01-01'), '[9,9]', 'bar')
;
DROP TABLE temporal3;

--
-- test changing the PK's dependencies
--

CREATE TABLE temporal3 (
	id int4range,
	valid_at daterange,
	CONSTRAINT temporal3_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);

ALTER TABLE temporal3 ALTER COLUMN valid_at DROP NOT NULL;
ALTER TABLE temporal3 ALTER COLUMN valid_at TYPE tstzrange USING tstzrange(lower(valid_at), upper(valid_at));
ALTER TABLE temporal3 RENAME COLUMN valid_at TO valid_thru;
ALTER TABLE temporal3 DROP COLUMN valid_thru;
DROP TABLE temporal3;

--
-- test PARTITION BY for ranges
--

-- temporal PRIMARY KEY:
CREATE TABLE temporal_partitioned (
	id int4range,
	valid_at daterange,
  name text,
	CONSTRAINT temporal_paritioned_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE tp1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,1]', '[2,2]');
CREATE TABLE tp2 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,3]', '[4,4]');
INSERT INTO temporal_partitioned VALUES
  ('[1,1]', daterange('2000-01-01', '2000-02-01'), 'one'),
  ('[1,1]', daterange('2000-02-01', '2000-03-01'), 'one'),
  ('[3,3]', daterange('2000-01-01', '2010-01-01'), 'three');
SELECT * FROM temporal_partitioned ORDER BY id, valid_at;
SELECT * FROM tp1 ORDER BY id, valid_at;
SELECT * FROM tp2 ORDER BY id, valid_at;
DROP TABLE temporal_partitioned;

-- temporal UNIQUE:
CREATE TABLE temporal_partitioned (
	id int4range,
	valid_at daterange,
  name text,
	CONSTRAINT temporal_paritioned_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE tp1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,1]', '[2,2]');
CREATE TABLE tp2 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,3]', '[4,4]');
INSERT INTO temporal_partitioned VALUES
  ('[1,1]', daterange('2000-01-01', '2000-02-01'), 'one'),
  ('[1,1]', daterange('2000-02-01', '2000-03-01'), 'one'),
  ('[3,3]', daterange('2000-01-01', '2010-01-01'), 'three');
SELECT * FROM temporal_partitioned ORDER BY id, valid_at;
SELECT * FROM tp1 ORDER BY id, valid_at;
SELECT * FROM tp2 ORDER BY id, valid_at;
DROP TABLE temporal_partitioned;

RESET datestyle;
