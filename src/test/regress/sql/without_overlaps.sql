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

-- PK from LIKE:
CREATE TABLE temporal_rng2 (LIKE temporal_rng INCLUDING ALL);
\d temporal_rng2
DROP TABLE temporal_rng2;

-- no PK from INHERITS:
CREATE TABLE temporal_rng2 () INHERITS (temporal_rng);
\d temporal_rng2
DROP TABLE temporal_rng2;
DROP TABLE temporal_rng;

-- PK in inheriting table:
CREATE TABLE temporal_rng (
  id int4range,
  valid_at daterange
);
CREATE TABLE temporal_rng2 (
  CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
) INHERITS (temporal_rng);
\d temporal_rng2
DROP TABLE temporal_rng CASCADE;

-- Add PK to already inheriting table:
CREATE TABLE temporal_rng (
  id int4range,
  valid_at daterange
);
CREATE TABLE temporal_rng2 () INHERITS (temporal_rng);
ALTER TABLE temporal_rng2
  ADD CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
\d temporal_rng2
DROP TABLE temporal_rng2;
DROP TABLE temporal_rng;

-- PK with two columns plus a range:
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

-- PK with one column plus a multirange:
CREATE TABLE temporal_mltrng (
  id int4range,
  valid_at datemultirange,
  CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_mltrng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_mltrng_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_mltrng_pk';

-- PK with two columns plus a multirange:
CREATE TABLE temporal_mltrng2 (
  id1 int4range,
  id2 int4range,
  valid_at datemultirange,
  CONSTRAINT temporal_mltrng2_pk PRIMARY KEY (id1, id2, valid_at WITHOUT OVERLAPS)
);
\d temporal_mltrng2
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_mltrng2_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_mltrng2_pk';

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
-- range PK: test with existing rows
--

ALTER TABLE temporal_rng DROP CONSTRAINT temporal_rng_pk;

-- okay:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-03'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-03-03', '2018-04-04'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[3,4)', daterange('2018-01-01', NULL));
ALTER TABLE temporal_rng ADD CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ALTER TABLE temporal_rng DROP CONSTRAINT temporal_rng_pk;

-- should fail:
BEGIN;
  INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-01-01', '2018-01-05'));
  ALTER TABLE temporal_rng ADD CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
-- rejects empty:
BEGIN;
  INSERT INTO temporal_rng (id, valid_at) VALUES ('[3,4)', 'empty');
  ALTER TABLE temporal_rng ADD CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
ALTER TABLE temporal_rng ADD CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
DELETE FROM temporal_rng;

--
-- range PK: test inserts
--

-- okay:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-03'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-03-03', '2018-04-04'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[3,4)', daterange('2018-01-01', NULL));

-- should fail:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng (id, valid_at) VALUES (NULL, daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[3,4)', NULL);
-- rejects empty:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[3,4)', 'empty');
SELECT * FROM temporal_rng ORDER BY id, valid_at;

--
-- range PK: test updates
--

-- update the scalar part
UPDATE  temporal_rng
SET     id = '[11,12)'
WHERE   id = '[1,2)'
AND     valid_at @> '2018-01-15'::date;
-- update the range part
UPDATE  temporal_rng
SET     valid_at = '[2020-01-01,2021-01-01)'
WHERE   id = '[11,12)'
AND     valid_at @> '2018-01-15'::date;
-- update both at once
UPDATE  temporal_rng
SET     id = '[21,22)',
        valid_at = '[2018-01-02,2018-02-03)'
WHERE   id = '[11,12)'
AND     valid_at @> '2020-01-15'::date;
SELECT * FROM temporal_rng ORDER BY id, valid_at;
-- should fail:
UPDATE  temporal_rng
SET     id = '[1,2)',
        valid_at = daterange('2018-03-05', '2018-05-05')
WHERE   id = '[21,22)';
-- set the scalar part to NULL
UPDATE  temporal_rng
SET     id = NULL,
        valid_at = daterange('2018-03-05', '2018-05-05')
WHERE   id = '[21,22)';
-- set the range part to NULL
UPDATE  temporal_rng
SET     id = '[1,2)',
        valid_at = NULL
WHERE   id = '[21,22)';
-- rejects empty:
UPDATE  temporal_rng
SET     id = '[1,2)',
        valid_at = 'empty'
WHERE   id = '[21,22)';
SELECT * FROM temporal_rng ORDER BY id, valid_at;

--
-- range UQ: test with existing rows
--

CREATE TABLE temporal_rng3 (
  id int4range,
  valid_at daterange
);

-- okay:
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-03'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-03-03', '2018-04-04'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[2,3)', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', daterange('2018-01-01', NULL));
INSERT INTO temporal_rng3 (id, valid_at) VALUES (NULL, daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', NULL);
ALTER TABLE temporal_rng3 ADD CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ALTER TABLE temporal_rng3 DROP CONSTRAINT temporal_rng3_uq;

-- should fail:
BEGIN;
  INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-01-01', '2018-01-05'));
  ALTER TABLE temporal_rng3 ADD CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
-- rejects empty:
BEGIN;
  INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', 'empty');
  ALTER TABLE temporal_rng3 ADD CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
ALTER TABLE temporal_rng3 ADD CONSTRAINT temporal_rng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
DELETE FROM temporal_rng3;

--
-- range UQ: test inserts
--

-- okay:
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-03'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-03-03', '2018-04-04'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[2,3)', daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', daterange('2018-01-01', NULL));
INSERT INTO temporal_rng3 (id, valid_at) VALUES (NULL, daterange('2018-01-01', '2018-01-05'));
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', NULL);

-- should fail:
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[1,2)', daterange('2018-01-01', '2018-01-05'));
-- rejects empty:
INSERT INTO temporal_rng3 (id, valid_at) VALUES ('[3,4)', 'empty');
SELECT * FROM temporal_rng3 ORDER BY id, valid_at;

--
-- range UQ: test updates
--

-- update the scalar part
UPDATE  temporal_rng3
SET     id = '[11,12)'
WHERE   id = '[1,2)'
AND     valid_at @> '2018-01-15'::date;
-- update the range part
UPDATE  temporal_rng3
SET     valid_at = '[2020-01-01,2021-01-01)'
WHERE   id = '[11,12)'
AND     valid_at @> '2018-01-15'::date;
-- update both at once
UPDATE  temporal_rng3
SET     id = '[21,22)',
        valid_at = '[2018-01-02,2018-02-03)'
WHERE   id = '[11,12)'
AND     valid_at @> '2020-01-15'::date;
-- set the scalar part to NULL
UPDATE  temporal_rng3
SET     id = NULL,
        valid_at = daterange('2020-01-01', '2021-01-01')
WHERE   id = '[21,22)';
-- set the range part to NULL
UPDATE  temporal_rng3
SET     id = '[1,2)',
        valid_at = NULL
WHERE   id IS NULL AND valid_at @> '2020-06-01'::date;
SELECT * FROM temporal_rng3 ORDER BY id, valid_at;
-- should fail:
UPDATE  temporal_rng3
SET     valid_at = daterange('2018-03-01', '2018-05-05')
WHERE   id = '[1,2)' AND valid_at IS NULL;
-- rejects empty:
UPDATE  temporal_rng3
SET     valid_at = 'empty'
WHERE   id = '[1,2)' AND valid_at IS NULL;
-- still rejects empty when scalar part is NULL:
UPDATE  temporal_rng3
SET     id = NULL,
        valid_at = 'empty'
WHERE   id = '[1,2)' AND valid_at IS NULL;
SELECT * FROM temporal_rng3 ORDER BY id, valid_at;
DROP TABLE temporal_rng3;

--
-- multirange PK: test with existing rows
--

ALTER TABLE temporal_mltrng DROP CONSTRAINT temporal_mltrng_pk;

-- okay:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[3,4)', datemultirange(daterange('2018-01-01', NULL)));
ALTER TABLE temporal_mltrng ADD CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ALTER TABLE temporal_mltrng DROP CONSTRAINT temporal_mltrng_pk;

-- should fail:
BEGIN;
  INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-01', '2018-01-05')));
  ALTER TABLE temporal_mltrng ADD CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
-- rejects empty:
BEGIN;
  INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[3,4)', '{}');
  ALTER TABLE temporal_mltrng ADD CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
ALTER TABLE temporal_mltrng ADD CONSTRAINT temporal_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
DELETE FROM temporal_mltrng;

--
-- multirange PK: test inserts
--

-- okay:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[3,4)', datemultirange(daterange('2018-01-01', NULL)));

-- should fail:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES (NULL, datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[3,4)', NULL);
-- rejects empty:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[3,4)', '{}');
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

--
-- multirange PK: test updates
--

-- update the scalar part
UPDATE  temporal_mltrng
SET     id = '[11,12)'
WHERE   id = '[1,2)'
AND     valid_at @> '2018-01-15'::date;
-- update the multirange part
UPDATE  temporal_mltrng
SET     valid_at = '{[2020-01-01,2021-01-01)}'
WHERE   id = '[11,12)'
AND     valid_at @> '2018-01-15'::date;
-- update both at once
UPDATE  temporal_mltrng
SET     id = '[21,22)',
        valid_at = '{[2018-01-02,2018-02-03)}'
WHERE   id = '[11,12)'
AND     valid_at @> '2020-01-15'::date;
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;
-- should fail:
UPDATE  temporal_mltrng
SET     id = '[1,2)',
        valid_at = datemultirange(daterange('2018-03-05', '2018-05-05'))
WHERE   id = '[21,22)';
-- set the scalar part to NULL
UPDATE  temporal_mltrng
SET     id = NULL,
        valid_at = datemultirange(daterange('2018-03-05', '2018-05-05'))
WHERE   id = '[21,22)';
-- set the multirange part to NULL
UPDATE  temporal_mltrng
SET     id = '[1,2)',
        valid_at = NULL
WHERE   id = '[21,22)';
-- rejects empty:
UPDATE  temporal_mltrng
SET     id = '[1,2)',
        valid_at = '{}'
WHERE   id = '[21,22)';
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

--
-- multirange UQ: test with existing rows
--

CREATE TABLE temporal_mltrng3 (
  id int4range,
  valid_at datemultirange
);

-- okay:
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', datemultirange(daterange('2018-01-01', NULL)));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES (NULL, datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', NULL);
ALTER TABLE temporal_mltrng3 ADD CONSTRAINT temporal_mltrng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ALTER TABLE temporal_mltrng3 DROP CONSTRAINT temporal_mltrng3_uq;

-- should fail:
BEGIN;
  INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-01', '2018-01-05')));
  ALTER TABLE temporal_mltrng3 ADD CONSTRAINT temporal_mltrng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
-- rejects empty:
BEGIN;
  INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', '{}');
  ALTER TABLE temporal_mltrng3 ADD CONSTRAINT temporal_mltrng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
ROLLBACK;
ALTER TABLE temporal_mltrng3 ADD CONSTRAINT temporal_mltrng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS);
DELETE FROM temporal_mltrng3;

--
-- multirange UQ: test inserts
--

-- okay:
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', datemultirange(daterange('2018-01-01', NULL)));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES (NULL, datemultirange(daterange('2018-01-01', '2018-01-05')));
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', NULL);

-- should fail:
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-01-01', '2018-01-05')));
-- rejects empty:
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[3,4)', '{}');
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

--
-- multirange UQ: test updates
--

-- update the scalar part
UPDATE  temporal_mltrng3
SET     id = '[11,12)'
WHERE   id = '[1,2)'
AND     valid_at @> '2018-01-15'::date;
-- update the multirange part
UPDATE  temporal_mltrng3
SET     valid_at = '{[2020-01-01,2021-01-01)}'
WHERE   id = '[11,12)'
AND     valid_at @> '2018-01-15'::date;
-- update both at once
UPDATE  temporal_mltrng3
SET     id = '[21,22)',
        valid_at = '{[2018-01-02,2018-02-03)}'
WHERE   id = '[11,12)'
AND     valid_at @> '2020-01-15'::date;
-- set the scalar part to NULL
UPDATE  temporal_mltrng3
SET     id = NULL,
        valid_at = datemultirange(daterange('2020-01-01', '2021-01-01'))
WHERE   id = '[21,22)';
-- set the multirange part to NULL
UPDATE  temporal_mltrng3
SET     id = '[1,2)',
        valid_at = NULL
WHERE   id IS NULL AND valid_at @> '2020-06-01'::date;
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;
-- should fail:
UPDATE  temporal_mltrng3
SET     valid_at = datemultirange(daterange('2018-03-01', '2018-05-05'))
WHERE   id = '[1,2)' AND valid_at IS NULL;
-- rejects empty:
UPDATE  temporal_mltrng3
SET     valid_at = '{}'
WHERE   id = '[1,2)' AND valid_at IS NULL;
-- still rejects empty when scalar part is NULL:
UPDATE  temporal_mltrng3
SET     id = NULL,
        valid_at = '{}'
WHERE   id = '[1,2)' AND valid_at IS NULL;
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;
DROP TABLE temporal_mltrng3;

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
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), '[7,8)', 'foo'),
  ('[2,3)', daterange('2000-01-01', '2010-01-01'), '[9,10)', 'bar')
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
CREATE TABLE tp1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,2)', '[2,3)');
CREATE TABLE tp2 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,4)', '[4,5)');
INSERT INTO temporal_partitioned (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2000-02-01'), 'one'),
  ('[1,2)', daterange('2000-02-01', '2000-03-01'), 'one'),
  ('[3,4)', daterange('2000-01-01', '2010-01-01'), 'three');
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
CREATE TABLE tp1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,2)', '[2,3)');
CREATE TABLE tp2 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,4)', '[4,5)');
INSERT INTO temporal_partitioned (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2000-02-01'), 'one'),
  ('[1,2)', daterange('2000-02-01', '2000-03-01'), 'one'),
  ('[3,4)', daterange('2000-01-01', '2010-01-01'), 'three');
SELECT * FROM temporal_partitioned ORDER BY id, valid_at;
SELECT * FROM tp1 ORDER BY id, valid_at;
SELECT * FROM tp2 ORDER BY id, valid_at;
DROP TABLE temporal_partitioned;

-- ALTER TABLE REPLICA IDENTITY
-- (should fail)
ALTER TABLE temporal_rng REPLICA IDENTITY USING INDEX temporal_rng_pk;

--
-- ON CONFLICT: ranges
--

TRUNCATE temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;
SELECT * FROM temporal_rng ORDER BY id, valid_at;

TRUNCATE temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
SELECT * FROM temporal_rng ORDER BY id, valid_at;

TRUNCATE temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO NOTHING;
SELECT * FROM temporal_rng ORDER BY id, valid_at;

TRUNCATE temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_rng ORDER BY id, valid_at;

TRUNCATE temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_rng (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal_rng_pk DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_rng ORDER BY id, valid_at;

-- with a UNIQUE constraint:

CREATE TABLE temporal3 (
  id int4range,
  valid_at daterange,
  CONSTRAINT temporal3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);
TRUNCATE temporal3;
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;
SELECT * FROM temporal3 ORDER BY id, valid_at;

TRUNCATE temporal3;
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO NOTHING;
SELECT * FROM temporal3 ORDER BY id, valid_at;

TRUNCATE temporal3;
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO NOTHING;
SELECT * FROM temporal3 ORDER BY id, valid_at;

TRUNCATE temporal3;
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal3 ORDER BY id, valid_at;

TRUNCATE temporal3;
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'));
-- with a conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2010-01-01', '2020-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal3 ORDER BY id, valid_at;

DROP TABLE temporal3;

--
-- ON CONFLICT: multiranges
--

TRUNCATE temporal_mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT DO NOTHING;
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

TRUNCATE temporal_mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

TRUNCATE temporal_mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO NOTHING;
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

TRUNCATE temporal_mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

TRUNCATE temporal_mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng_pk DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_mltrng ORDER BY id, valid_at;

-- with a UNIQUE constraint:

CREATE TABLE temporal_mltrng3 (
  id int4range,
  valid_at datemultirange,
  CONSTRAINT temporal_mltrng3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
);
TRUNCATE temporal_mltrng3;
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT DO NOTHING;
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

TRUNCATE temporal_mltrng3;
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO NOTHING;
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

TRUNCATE temporal_mltrng3;
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO NOTHING;
-- id matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO NOTHING;
-- date matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO NOTHING;
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

TRUNCATE temporal_mltrng3;
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT (id, valid_at) DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

TRUNCATE temporal_mltrng3;
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2000-01-01', '2010-01-01')));
-- with a conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO UPDATE SET id = EXCLUDED.id + '[2,3)';
-- id matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2010-01-01', '2020-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO UPDATE SET id = EXCLUDED.id + '[3,4)';
-- date matches but no conflict
INSERT INTO temporal_mltrng3 (id, valid_at) VALUES ('[2,3)', datemultirange(daterange('2005-01-01', '2006-01-01'))) ON CONFLICT ON CONSTRAINT temporal_mltrng3_uq DO UPDATE SET id = EXCLUDED.id + '[4,5)';
SELECT * FROM temporal_mltrng3 ORDER BY id, valid_at;

DROP TABLE temporal_mltrng3;

RESET datestyle;
