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
\d temporal_rng
ALTER TABLE temporal_rng REPLICA IDENTITY USING INDEX temporal_rng_pk;
\d temporal_rng

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

--
-- test FK dependencies
--

-- can't drop a range referenced by an FK, unless with CASCADE
CREATE TABLE temporal3 (
  id int4range,
  valid_at daterange,
  CONSTRAINT temporal3_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal3 (id, PERIOD valid_at)
);
ALTER TABLE temporal3 DROP COLUMN valid_at;
ALTER TABLE temporal3 DROP COLUMN valid_at CASCADE;
DROP TABLE temporal_fk_rng2rng;
DROP TABLE temporal3;

--
-- test FOREIGN KEY, range references range
--

-- test table setup
DROP TABLE temporal_rng;
CREATE TABLE temporal_rng (id int4range, valid_at daterange);
ALTER TABLE temporal_rng
  ADD CONSTRAINT temporal_rng_pk
  PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);

-- Can't create a FK with a mismatched range type
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at int4range,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk2 PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk2 FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);

-- works: PERIOD for both referenced and referencing
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);
DROP TABLE temporal_fk_rng2rng;

-- with mismatched PERIOD columns:

-- (parent_id, PERIOD valid_at) REFERENCES (id, valid_at)
-- REFERENCES part should specify PERIOD
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id, valid_at)
);
-- (parent_id, valid_at) REFERENCES (id, valid_at)
-- both should specify PERIOD:
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_rng (id, valid_at)
);
-- (parent_id, valid_at) REFERENCES (id, PERIOD valid_at)
-- FOREIGN KEY part should specify PERIOD
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);
-- (parent_id, valid_at) REFERENCES [implicit]
-- FOREIGN KEY part should specify PERIOD
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_rng
);
-- (parent_id, PERIOD valid_at) REFERENCES (id)
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id)
);
-- (parent_id) REFERENCES (id, PERIOD valid_at)
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);
-- with inferred PK on the referenced table:
-- (parent_id, PERIOD valid_at) REFERENCES [implicit]
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng
);
DROP TABLE temporal_fk_rng2rng;
-- (parent_id) REFERENCES [implicit]
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id)
    REFERENCES temporal_rng
);

-- should fail because of duplicate referenced columns:
CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD parent_id)
    REFERENCES temporal_rng (id, PERIOD id)
);

-- Two scalar columns
DROP TABLE temporal_rng2;
CREATE TABLE temporal_rng2 (
  id1 int4range,
  id2 int4range,
  valid_at daterange,
  CONSTRAINT temporal_rng2_pk PRIMARY KEY (id1, id2, valid_at WITHOUT OVERLAPS)
);

CREATE TABLE temporal_fk2_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id1 int4range,
  parent_id2 int4range,
  CONSTRAINT temporal_fk2_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk2_rng2rng_fk FOREIGN KEY (parent_id1, parent_id2, PERIOD valid_at)
    REFERENCES temporal_rng2 (id1, id2, PERIOD valid_at)
);
\d temporal_fk2_rng2rng
DROP TABLE temporal_fk2_rng2rng;

--
-- test ALTER TABLE ADD CONSTRAINT
--

CREATE TABLE temporal_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng (id, PERIOD valid_at);
-- Two scalar columns:
CREATE TABLE temporal_fk2_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id1 int4range,
  parent_id2 int4range,
  CONSTRAINT temporal_fk2_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
ALTER TABLE temporal_fk2_rng2rng
  ADD CONSTRAINT temporal_fk2_rng2rng_fk
  FOREIGN KEY (parent_id1, parent_id2, PERIOD valid_at)
  REFERENCES temporal_rng2 (id1, id2, PERIOD valid_at);
\d temporal_fk2_rng2rng

-- with inferred PK on the referenced table, and wrong column type:
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk,
  ALTER COLUMN valid_at TYPE tsrange USING tsrange(lower(valid_at), upper(valid_at));
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;
ALTER TABLE temporal_fk_rng2rng
  ALTER COLUMN valid_at TYPE daterange USING daterange(lower(valid_at)::date, upper(valid_at)::date);

-- with inferred PK on the referenced table:
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;

-- should fail because of duplicate referenced columns:
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk2
  FOREIGN KEY (parent_id, PERIOD parent_id)
  REFERENCES temporal_rng (id, PERIOD id);

--
-- test with rows already
--

DELETE FROM temporal_fk_rng2rng;
DELETE FROM temporal_rng;
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[1,2)', daterange('2018-01-02', '2018-02-03')),
  ('[1,2)', daterange('2018-03-03', '2018-04-04')),
  ('[2,3)', daterange('2018-01-01', '2018-01-05')),
  ('[3,4)', daterange('2018-01-01', NULL));

ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-01'), '[1,2)');
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[2,3)', daterange('2018-01-02', '2018-04-01'), '[1,2)');
-- should fail:
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;
-- okay again:
DELETE FROM temporal_fk_rng2rng;
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;

--
-- test pg_get_constraintdef
--

SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_fk_rng2rng_fk';

--
-- test FK referencing inserts
--

INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[1,2)', daterange('2018-01-02', '2018-02-01'), '[1,2)');
-- should fail:
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[2,3)', daterange('2018-01-02', '2018-04-01'), '[1,2)');
-- now it should work:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[1,2)', daterange('2018-02-03', '2018-03-03'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[2,3)', daterange('2018-01-02', '2018-04-01'), '[1,2)');

--
-- test FK referencing updates
--

-- slide the edge across a referenced transition:
UPDATE temporal_fk_rng2rng SET valid_at = daterange('2018-01-02', '2018-02-20') WHERE id = '[1,2)';
-- should fail:
UPDATE temporal_fk_rng2rng SET valid_at = daterange('2018-01-02', '2018-05-01') WHERE id = '[1,2)';
UPDATE temporal_fk_rng2rng SET parent_id = '[8,9)' WHERE id = '[1,2)';

-- ALTER FK DEFERRABLE

BEGIN;
  INSERT INTO temporal_rng (id, valid_at) VALUES
    ('[5,6)', daterange('2018-01-01', '2018-02-01')),
    ('[5,6)', daterange('2018-02-01', '2018-03-01'));
  INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
    ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
  ALTER TABLE temporal_fk_rng2rng
    ALTER CONSTRAINT temporal_fk_rng2rng_fk
    DEFERRABLE INITIALLY DEFERRED;

  DELETE FROM temporal_rng WHERE id = '[5,6)'; --should not fail yet.
COMMIT; -- should fail here.

--
-- test FK referenced updates NO ACTION
--

TRUNCATE temporal_rng, temporal_fk_rng2rng;
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng
  ON UPDATE NO ACTION;
-- a PK update that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01') WHERE id = '[5,6)';
-- a PK update that succeeds even though the numeric id is referenced because the range isn't:
DELETE FROM temporal_rng WHERE id = '[5,6)';
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[5,6)', daterange('2018-01-01', '2018-02-01')),
  ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id)
  VALUES ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
UPDATE temporal_rng SET valid_at = daterange('2016-02-01', '2016-03-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- A PK update sliding the edge between two referenced rows:
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[6,7)', daterange('2018-01-01', '2018-02-01')),
  ('[6,7)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[4,5)', daterange('2018-01-15', '2018-02-15'), '[6,7)');
UPDATE temporal_rng
  SET valid_at = CASE WHEN lower(valid_at) = '2018-01-01' THEN daterange('2018-01-01', '2018-01-05')
                      WHEN lower(valid_at) = '2018-02-01' THEN daterange('2018-01-05', '2018-03-01') END
  WHERE id = '[6,7)';
-- a PK update shrinking the referenced range but still valid:
-- There are two references: one fulfilled by the first pk row,
-- the other fulfilled by both pk rows combined.
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[1,2)', daterange('2018-01-01', '2018-03-01')),
  ('[1,2)', daterange('2018-03-01', '2018-06-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[1,2)', daterange('2018-01-15', '2018-02-01'), '[1,2)'),
  ('[2,3)', daterange('2018-01-15', '2018-05-01'), '[1,2)');
UPDATE temporal_rng SET valid_at = daterange('2018-01-15', '2018-03-01')
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update growing the referenced range is fine:
UPDATE temporal_rng SET valid_at = daterange('2018-01-01', '2018-03-01')
  WHERE id = '[1,2)' AND valid_at @> '2018-01-25'::date;
-- a PK update shrinking the referenced range and changing the id invalidates the whole range (error):
UPDATE temporal_rng SET id = '[2,3)', valid_at = daterange('2018-01-15', '2018-03-01')
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update changing only the id invalidates the whole range (error):
UPDATE temporal_rng SET id = '[2,3)'
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update that loses time from both ends, but is still valid:
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[2,3)', daterange('2018-01-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[5,6)', daterange('2018-01-15', '2018-02-01'), '[2,3)');
UPDATE temporal_rng SET valid_at = daterange('2018-01-15', '2018-02-15')
  WHERE id = '[2,3)';
-- a PK update that fails because both are referenced:
UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
-- a PK update that fails because both are referenced, but not 'til commit:
BEGIN;
  ALTER TABLE temporal_fk_rng2rng
    ALTER CONSTRAINT temporal_fk_rng2rng_fk
    DEFERRABLE INITIALLY DEFERRED;

  UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
    WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
COMMIT;
-- changing the scalar part fails:
UPDATE temporal_rng SET id = '[7,8)'
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
-- then delete the objecting FK record and the same PK update succeeds:
DELETE FROM temporal_fk_rng2rng WHERE id = '[3,4)';
UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- test FK referenced updates RESTRICT
--

TRUNCATE temporal_rng, temporal_fk_rng2rng;
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng
  ON UPDATE RESTRICT;
-- a PK update that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01') WHERE id = '[5,6)';
-- a PK update that succeeds even though the numeric id is referenced because the range isn't:
DELETE FROM temporal_rng WHERE id = '[5,6)';
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[5,6)', daterange('2018-01-01', '2018-02-01')),
  ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
UPDATE temporal_rng SET valid_at = daterange('2016-02-01', '2016-03-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- A PK update sliding the edge between two referenced rows:
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[6,7)', daterange('2018-01-01', '2018-02-01')),
  ('[6,7)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[4,5)', daterange('2018-01-15', '2018-02-15'), '[6,7)');
UPDATE temporal_rng
  SET valid_at = CASE WHEN lower(valid_at) = '2018-01-01' THEN daterange('2018-01-01', '2018-01-05')
                      WHEN lower(valid_at) = '2018-02-01' THEN daterange('2018-01-05', '2018-03-01') END
  WHERE id = '[6,7)';
-- a PK update that fails because both are referenced (even before commit):
BEGIN;
  ALTER TABLE temporal_fk_rng2rng
    ALTER CONSTRAINT temporal_fk_rng2rng_fk
    DEFERRABLE INITIALLY DEFERRED;
  UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
ROLLBACK;
-- changing the scalar part fails:
UPDATE temporal_rng SET id = '[7,8)'
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
-- then delete the objecting FK record and the same PK update succeeds:
DELETE FROM temporal_fk_rng2rng WHERE id = '[3,4)';
UPDATE temporal_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- test FK referenced deletes NO ACTION
--

TRUNCATE temporal_rng, temporal_fk_rng2rng;
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng;
-- a PK delete that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
DELETE FROM temporal_rng WHERE id = '[5,6)';
-- a PK delete that succeeds even though the numeric id is referenced because the range isn't:
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[5,6)', daterange('2018-01-01', '2018-02-01')),
  ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- a PK delete that fails because both are referenced:
DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
-- a PK delete that fails because both are referenced, but not 'til commit:
BEGIN;
  ALTER TABLE temporal_fk_rng2rng
    ALTER CONSTRAINT temporal_fk_rng2rng_fk
    DEFERRABLE INITIALLY DEFERRED;

  DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
COMMIT;
-- then delete the objecting FK record and the same PK delete succeeds:
DELETE FROM temporal_fk_rng2rng WHERE id = '[3,4)';
DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- test FK referenced deletes RESTRICT
--

TRUNCATE temporal_rng, temporal_fk_rng2rng;
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk;
ALTER TABLE temporal_fk_rng2rng
  ADD CONSTRAINT temporal_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_rng
  ON DELETE RESTRICT;
INSERT INTO temporal_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
DELETE FROM temporal_rng WHERE id = '[5,6)';
-- a PK delete that succeeds even though the numeric id is referenced because the range isn't:
INSERT INTO temporal_rng (id, valid_at) VALUES
  ('[5,6)', daterange('2018-01-01', '2018-02-01')),
  ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- a PK delete that fails because both are referenced (even before commit):
BEGIN;
  ALTER TABLE temporal_fk_rng2rng
    ALTER CONSTRAINT temporal_fk_rng2rng_fk
    DEFERRABLE INITIALLY DEFERRED;
  DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');
ROLLBACK;
-- then delete the objecting FK record and the same PK delete succeeds:
DELETE FROM temporal_fk_rng2rng WHERE id = '[3,4)';
DELETE FROM temporal_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- test ON UPDATE/DELETE options
--

-- test FK referenced updates CASCADE
INSERT INTO temporal_rng (id, valid_at) VALUES ('[6,7)', daterange('2018-01-01', '2021-01-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[4,5)', daterange('2018-01-01', '2021-01-01'), '[6,7)');
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng
    ON DELETE CASCADE ON UPDATE CASCADE;

-- test FK referenced updates SET NULL
INSERT INTO temporal_rng (id, valid_at) VALUES ('[9,10)', daterange('2018-01-01', '2021-01-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[6,7)', daterange('2018-01-01', '2021-01-01'), '[9,10)');
ALTER TABLE temporal_fk_rng2rng
  DROP CONSTRAINT temporal_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng
    ON DELETE SET NULL ON UPDATE SET NULL;

-- test FK referenced updates SET DEFAULT
INSERT INTO temporal_rng (id, valid_at) VALUES ('[-1,-1]', daterange(null, null));
INSERT INTO temporal_rng (id, valid_at) VALUES ('[12,13)', daterange('2018-01-01', '2021-01-01'));
INSERT INTO temporal_fk_rng2rng (id, valid_at, parent_id) VALUES ('[8,9)', daterange('2018-01-01', '2021-01-01'), '[12,13)');
ALTER TABLE temporal_fk_rng2rng
  ALTER COLUMN parent_id SET DEFAULT '[-1,-1]',
  DROP CONSTRAINT temporal_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng
    ON DELETE SET DEFAULT ON UPDATE SET DEFAULT;

--
-- test FOREIGN KEY, multirange references multirange
--

-- test table setup
DROP TABLE temporal_mltrng;
CREATE TABLE temporal_mltrng ( id int4range, valid_at datemultirange);
ALTER TABLE temporal_mltrng
  ADD CONSTRAINT temporal_mltrng_pk
  PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);

-- Can't create a FK with a mismatched multirange type
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at int4multirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk2 PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk2 FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_mltrng (id, PERIOD valid_at)
);

CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_mltrng (id, PERIOD valid_at)
);
DROP TABLE temporal_fk_mltrng2mltrng;

-- with mismatched PERIOD columns:

-- (parent_id, PERIOD valid_at) REFERENCES (id, valid_at)
-- REFERENCES part should specify PERIOD
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_mltrng (id, valid_at)
);
-- (parent_id, valid_at) REFERENCES (id, valid_at)
-- both should specify PERIOD:
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_mltrng (id, valid_at)
);
-- (parent_id, valid_at) REFERENCES (id, PERIOD valid_at)
-- FOREIGN KEY part should specify PERIOD
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_mltrng (id, PERIOD valid_at)
);
-- (parent_id, valid_at) REFERENCES [implicit]
-- FOREIGN KEY part should specify PERIOD
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, valid_at)
    REFERENCES temporal_mltrng
);
-- (parent_id, PERIOD valid_at) REFERENCES (id)
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_mltrng (id)
);
-- (parent_id) REFERENCES (id, PERIOD valid_at)
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id)
    REFERENCES temporal_mltrng (id, PERIOD valid_at)
);
-- with inferred PK on the referenced table:
-- (parent_id, PERIOD valid_at) REFERENCES [implicit]
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_mltrng
);
DROP TABLE temporal_fk_mltrng2mltrng;
-- (parent_id) REFERENCES [implicit]
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id)
    REFERENCES temporal_mltrng
);

-- should fail because of duplicate referenced columns:
CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD parent_id)
    REFERENCES temporal_mltrng (id, PERIOD id)
);

-- Two scalar columns
DROP TABLE temporal_mltrng2;
CREATE TABLE temporal_mltrng2 (
  id1 int4range,
  id2 int4range,
  valid_at datemultirange,
  CONSTRAINT temporal_mltrng2_pk PRIMARY KEY (id1, id2, valid_at WITHOUT OVERLAPS)
);

CREATE TABLE temporal_fk2_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id1 int4range,
  parent_id2 int4range,
  CONSTRAINT temporal_fk2_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk2_mltrng2mltrng_fk FOREIGN KEY (parent_id1, parent_id2, PERIOD valid_at)
    REFERENCES temporal_mltrng2 (id1, id2, PERIOD valid_at)
);
\d temporal_fk2_mltrng2mltrng
DROP TABLE temporal_fk2_mltrng2mltrng;

--
-- test ALTER TABLE ADD CONSTRAINT
--

CREATE TABLE temporal_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at);

-- Two scalar columns:
CREATE TABLE temporal_fk2_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id1 int4range,
  parent_id2 int4range,
  CONSTRAINT temporal_fk2_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);

ALTER TABLE temporal_fk2_mltrng2mltrng
  ADD CONSTRAINT temporal_fk2_mltrng2mltrng_fk
  FOREIGN KEY (parent_id1, parent_id2, PERIOD valid_at)
  REFERENCES temporal_mltrng2 (id1, id2, PERIOD valid_at);
\d temporal_fk2_mltrng2mltrng

-- should fail because of duplicate referenced columns:
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk2
  FOREIGN KEY (parent_id, PERIOD parent_id)
  REFERENCES temporal_mltrng (id, PERIOD id);

--
-- test with rows already
--

DELETE FROM temporal_fk_mltrng2mltrng;
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03'))),
  ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04'))),
  ('[2,3)', datemultirange(daterange('2018-01-01', '2018-01-05'))),
  ('[3,4)', datemultirange(daterange('2018-01-01', NULL)));

ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-01')), '[1,2)');
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at);
ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[2,3)', datemultirange(daterange('2018-01-02', '2018-04-01')), '[1,2)');
-- should fail:
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at);
-- okay again:
DELETE FROM temporal_fk_mltrng2mltrng;
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at);

--
-- test pg_get_constraintdef
--

SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_fk_mltrng2mltrng_fk';

--
-- test FK referencing inserts
--

INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-01')), '[1,2)');
-- should fail:
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[2,3)', datemultirange(daterange('2018-01-02', '2018-04-01')), '[1,2)');
-- now it should work:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[1,2)', datemultirange(daterange('2018-02-03', '2018-03-03')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[2,3)', datemultirange(daterange('2018-01-02', '2018-04-01')), '[1,2)');

--
-- test FK referencing updates
--

-- slide the edge across a referenced transition:
UPDATE temporal_fk_mltrng2mltrng SET valid_at = datemultirange(daterange('2018-01-02', '2018-02-20')) WHERE id = '[1,2)';
-- should fail:
UPDATE temporal_fk_mltrng2mltrng SET valid_at = datemultirange(daterange('2018-01-02', '2018-05-01')) WHERE id = '[1,2)';
UPDATE temporal_fk_mltrng2mltrng SET parent_id = '[8,9)' WHERE id = '[1,2)';

-- ALTER FK DEFERRABLE

BEGIN;
  INSERT INTO temporal_mltrng (id, valid_at) VALUES
    ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
    ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
  INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
    ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
  ALTER TABLE temporal_fk_mltrng2mltrng
    ALTER CONSTRAINT temporal_fk_mltrng2mltrng_fk
    DEFERRABLE INITIALLY DEFERRED;

  DELETE FROM temporal_mltrng WHERE id = '[5,6)'; --should not fail yet.
COMMIT; -- should fail here.

--
-- test FK referenced updates NO ACTION
--

TRUNCATE temporal_mltrng, temporal_fk_mltrng2mltrng;
ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at)
  ON UPDATE NO ACTION;
-- a PK update that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01')) WHERE id = '[5,6)';
-- a PK update that succeeds even though the numeric id is referenced because the range isn't:
DELETE FROM temporal_mltrng WHERE id = '[5,6)';
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-02-01', '2016-03-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- A PK update sliding the edge between two referenced rows:
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[6,7)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[6,7)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[4,5)', datemultirange(daterange('2018-01-15', '2018-02-15')), '[6,7)');
UPDATE temporal_mltrng
  SET valid_at = CASE WHEN lower(valid_at) = '2018-01-01' THEN datemultirange(daterange('2018-01-01', '2018-01-05'))
                      WHEN lower(valid_at) = '2018-02-01' THEN datemultirange(daterange('2018-01-05', '2018-03-01')) END
  WHERE id = '[6,7)';
-- a PK update shrinking the referenced multirange but still valid:
-- There are two references: one fulfilled by the first pk row,
-- the other fulfilled by both pk rows combined.
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[1,2)', datemultirange(daterange('2018-01-01', '2018-03-01'))),
  ('[1,2)', datemultirange(daterange('2018-03-01', '2018-06-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[1,2)', datemultirange(daterange('2018-01-15', '2018-02-01')), '[1,2)'),
  ('[2,3)', datemultirange(daterange('2018-01-15', '2018-05-01')), '[1,2)');
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2018-01-15', '2018-03-01'))
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update growing the referenced multirange is fine:
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2018-01-01', '2018-03-01'))
  WHERE id = '[1,2)' AND valid_at @> '2018-01-25'::date;
-- a PK update shrinking the referenced multirange and changing the id invalidates the whole multirange (error):
UPDATE temporal_mltrng SET id = '[2,3)', valid_at = datemultirange(daterange('2018-01-15', '2018-03-01'))
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update changing only the id invalidates the whole multirange (error):
UPDATE temporal_mltrng SET id = '[2,3)'
  WHERE id = '[1,2)' AND valid_at @> '2018-01-15'::date;
-- a PK update that loses time from both ends, but is still valid:
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[2,3)', datemultirange(daterange('2018-01-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[5,6)', datemultirange(daterange('2018-01-15', '2018-02-01')), '[2,3)');
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2018-01-15', '2018-02-15'))
  WHERE id = '[2,3)';
-- a PK update that fails because both are referenced:
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
-- a PK update that fails because both are referenced, but not 'til commit:
BEGIN;
  ALTER TABLE temporal_fk_mltrng2mltrng
    ALTER CONSTRAINT temporal_fk_mltrng2mltrng_fk
    DEFERRABLE INITIALLY DEFERRED;

  UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
COMMIT;
-- changing the scalar part fails:
UPDATE temporal_mltrng SET id = '[7,8)'
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- test FK referenced updates RESTRICT
--

TRUNCATE temporal_mltrng, temporal_fk_mltrng2mltrng;
ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at)
  ON UPDATE RESTRICT;
-- a PK update that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01')) WHERE id = '[5,6)';
-- a PK update that succeeds even though the numeric id is referenced because the range isn't:
DELETE FROM temporal_mltrng WHERE id = '[5,6)';
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-02-01', '2016-03-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- A PK update sliding the edge between two referenced rows:
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[6,7)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[6,7)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[4,5)', datemultirange(daterange('2018-01-15', '2018-02-15')), '[6,7)');
UPDATE temporal_mltrng
  SET valid_at = CASE WHEN lower(valid_at) = '2018-01-01' THEN datemultirange(daterange('2018-01-01', '2018-01-05'))
                      WHEN lower(valid_at) = '2018-02-01' THEN datemultirange(daterange('2018-01-05', '2018-03-01')) END
  WHERE id = '[6,7)';
-- a PK update that fails because both are referenced (even before commit):
BEGIN;
  ALTER TABLE temporal_fk_mltrng2mltrng
    ALTER CONSTRAINT temporal_fk_mltrng2mltrng_fk
    DEFERRABLE INITIALLY DEFERRED;

  UPDATE temporal_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
ROLLBACK;
-- changing the scalar part fails:
UPDATE temporal_mltrng SET id = '[7,8)'
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- test FK referenced deletes NO ACTION
--

TRUNCATE temporal_mltrng, temporal_fk_mltrng2mltrng;
ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at);
-- a PK delete that succeeds because the numeric id isn't referenced:
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
DELETE FROM temporal_mltrng WHERE id = '[5,6)';
-- a PK delete that succeeds even though the numeric id is referenced because the range isn't:
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
DELETE FROM temporal_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- a PK delete that fails because both are referenced:
DELETE FROM temporal_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
-- a PK delete that fails because both are referenced, but not 'til commit:
BEGIN;
  ALTER TABLE temporal_fk_mltrng2mltrng
    ALTER CONSTRAINT temporal_fk_mltrng2mltrng_fk
    DEFERRABLE INITIALLY DEFERRED;

  DELETE FROM temporal_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
COMMIT;

--
-- test FK referenced deletes RESTRICT
--

TRUNCATE temporal_mltrng, temporal_fk_mltrng2mltrng;
ALTER TABLE temporal_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_fk_mltrng2mltrng_fk;
ALTER TABLE temporal_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_mltrng (id, PERIOD valid_at)
  ON DELETE RESTRICT;
INSERT INTO temporal_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
DELETE FROM temporal_mltrng WHERE id = '[5,6)';
-- a PK delete that succeeds even though the numeric id is referenced because the range isn't:
INSERT INTO temporal_mltrng (id, valid_at) VALUES
  ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01'))),
  ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
DELETE FROM temporal_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- a PK delete that fails because both are referenced (even before commit):
BEGIN;
  ALTER TABLE temporal_fk_mltrng2mltrng
    ALTER CONSTRAINT temporal_fk_mltrng2mltrng_fk
    DEFERRABLE INITIALLY DEFERRED;

  DELETE FROM temporal_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));
ROLLBACK;

--
-- FK between partitioned tables: ranges
--

CREATE TABLE temporal_partitioned_rng (
  id int4range,
  valid_at daterange,
  name text,
  CONSTRAINT temporal_paritioned_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE tp1 partition OF temporal_partitioned_rng FOR VALUES IN ('[1,2)', '[3,4)', '[5,6)', '[7,8)', '[9,10)', '[11,12)');
CREATE TABLE tp2 partition OF temporal_partitioned_rng FOR VALUES IN ('[2,3)', '[4,5)', '[6,7)', '[8,9)', '[10,11)', '[12,13)');
INSERT INTO temporal_partitioned_rng (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2000-02-01'), 'one'),
  ('[1,2)', daterange('2000-02-01', '2000-03-01'), 'one'),
  ('[2,3)', daterange('2000-01-01', '2010-01-01'), 'two');

CREATE TABLE temporal_partitioned_fk_rng2rng (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  CONSTRAINT temporal_partitioned_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_partitioned_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_rng (id, PERIOD valid_at)
) PARTITION BY LIST (id);
CREATE TABLE tfkp1 partition OF temporal_partitioned_fk_rng2rng FOR VALUES IN ('[1,2)', '[3,4)', '[5,6)', '[7,8)', '[9,10)', '[11,12)');
CREATE TABLE tfkp2 partition OF temporal_partitioned_fk_rng2rng FOR VALUES IN ('[2,3)', '[4,5)', '[6,7)', '[8,9)', '[10,11)', '[12,13)');

--
-- partitioned FK referencing inserts
--

INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[1,2)', daterange('2000-01-01', '2000-02-15'), '[1,2)'),
  ('[1,2)', daterange('2001-01-01', '2002-01-01'), '[2,3)'),
  ('[2,3)', daterange('2000-01-01', '2000-02-15'), '[1,2)');
-- should fail:
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[3,4)', daterange('2010-01-01', '2010-02-15'), '[1,2)');
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES
  ('[3,4)', daterange('2000-01-01', '2000-02-15'), '[3,4)');

--
-- partitioned FK referencing updates
--

UPDATE temporal_partitioned_fk_rng2rng SET valid_at = daterange('2000-01-01', '2000-02-13') WHERE id = '[2,3)';
-- move a row from the first partition to the second
UPDATE temporal_partitioned_fk_rng2rng SET id = '[4,5)' WHERE id = '[1,2)';
-- move a row from the second partition to the first
UPDATE temporal_partitioned_fk_rng2rng SET id = '[1,2)' WHERE id = '[4,5)';
-- should fail:
UPDATE temporal_partitioned_fk_rng2rng SET valid_at = daterange('2000-01-01', '2000-04-01') WHERE id = '[1,2)';

--
-- partitioned FK referenced updates NO ACTION
--

TRUNCATE temporal_partitioned_rng, temporal_partitioned_fk_rng2rng;
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2016-01-01', '2016-02-01'));
UPDATE temporal_partitioned_rng SET valid_at = daterange('2018-01-01', '2018-02-01') WHERE id = '[5,6)';
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
UPDATE temporal_partitioned_rng SET valid_at = daterange('2016-02-01', '2016-03-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- should fail:
UPDATE temporal_partitioned_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- partitioned FK referenced deletes NO ACTION
--

TRUNCATE temporal_partitioned_rng, temporal_partitioned_fk_rng2rng;
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
DELETE FROM temporal_partitioned_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- should fail:
DELETE FROM temporal_partitioned_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- partitioned FK referenced updates RESTRICT
--

TRUNCATE temporal_partitioned_rng, temporal_partitioned_fk_rng2rng;
ALTER TABLE temporal_partitioned_fk_rng2rng
  DROP CONSTRAINT temporal_partitioned_fk_rng2rng_fk;
ALTER TABLE temporal_partitioned_fk_rng2rng
  ADD CONSTRAINT temporal_partitioned_fk_rng2rng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_partitioned_rng
  ON DELETE RESTRICT;
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2016-01-01', '2016-02-01'));
UPDATE temporal_partitioned_rng SET valid_at = daterange('2018-01-01', '2018-02-01') WHERE id = '[5,6)';
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
UPDATE temporal_partitioned_rng SET valid_at = daterange('2016-02-01', '2016-03-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- should fail:
UPDATE temporal_partitioned_rng SET valid_at = daterange('2016-01-01', '2016-02-01')
  WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- partitioned FK referenced deletes RESTRICT
--

TRUNCATE temporal_partitioned_rng, temporal_partitioned_fk_rng2rng;
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-01-01', '2018-02-01'));
INSERT INTO temporal_partitioned_rng (id, valid_at) VALUES ('[5,6)', daterange('2018-02-01', '2018-03-01'));
INSERT INTO temporal_partitioned_fk_rng2rng (id, valid_at, parent_id) VALUES ('[3,4)', daterange('2018-01-05', '2018-01-10'), '[5,6)');
DELETE FROM temporal_partitioned_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-02-01', '2018-03-01');
-- should fail:
DELETE FROM temporal_partitioned_rng WHERE id = '[5,6)' AND valid_at = daterange('2018-01-01', '2018-02-01');

--
-- partitioned FK referenced updates CASCADE
--

ALTER TABLE temporal_partitioned_fk_rng2rng
  DROP CONSTRAINT temporal_partitioned_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_rng
    ON DELETE CASCADE ON UPDATE CASCADE;

--
-- partitioned FK referenced deletes CASCADE
--

--
-- partitioned FK referenced updates SET NULL
--

ALTER TABLE temporal_partitioned_fk_rng2rng
  DROP CONSTRAINT temporal_partitioned_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_rng
    ON DELETE SET NULL ON UPDATE SET NULL;

--
-- partitioned FK referenced deletes SET NULL
--

--
-- partitioned FK referenced updates SET DEFAULT
--

ALTER TABLE temporal_partitioned_fk_rng2rng
  ALTER COLUMN parent_id SET DEFAULT '[-1,-1]',
  DROP CONSTRAINT temporal_partitioned_fk_rng2rng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_rng2rng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_rng
    ON DELETE SET DEFAULT ON UPDATE SET DEFAULT;

--
-- partitioned FK referenced deletes SET DEFAULT
--

DROP TABLE temporal_partitioned_fk_rng2rng;
DROP TABLE temporal_partitioned_rng;

--
-- FK between partitioned tables: multiranges
--

CREATE TABLE temporal_partitioned_mltrng (
  id int4range,
  valid_at datemultirange,
  name text,
  CONSTRAINT temporal_paritioned_mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE tp1 PARTITION OF temporal_partitioned_mltrng FOR VALUES IN ('[1,2)', '[3,4)', '[5,6)', '[7,8)', '[9,10)', '[11,12)', '[13,14)', '[15,16)', '[17,18)', '[19,20)', '[21,22)', '[23,24)');
CREATE TABLE tp2 PARTITION OF temporal_partitioned_mltrng FOR VALUES IN ('[0,1)', '[2,3)', '[4,5)', '[6,7)', '[8,9)', '[10,11)', '[12,13)', '[14,15)', '[16,17)', '[18,19)', '[20,21)', '[22,23)', '[24,25)');
INSERT INTO temporal_partitioned_mltrng (id, valid_at, name) VALUES
  ('[1,2)', datemultirange(daterange('2000-01-01', '2000-02-01')), 'one'),
  ('[1,2)', datemultirange(daterange('2000-02-01', '2000-03-01')), 'one'),
  ('[2,3)', datemultirange(daterange('2000-01-01', '2010-01-01')), 'two');

CREATE TABLE temporal_partitioned_fk_mltrng2mltrng (
  id int4range,
  valid_at datemultirange,
  parent_id int4range,
  CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_mltrng (id, PERIOD valid_at)
) PARTITION BY LIST (id);
CREATE TABLE tfkp1 PARTITION OF temporal_partitioned_fk_mltrng2mltrng FOR VALUES IN ('[1,2)', '[3,4)', '[5,6)', '[7,8)', '[9,10)', '[11,12)', '[13,14)', '[15,16)', '[17,18)', '[19,20)', '[21,22)', '[23,24)');
CREATE TABLE tfkp2 PARTITION OF temporal_partitioned_fk_mltrng2mltrng FOR VALUES IN ('[0,1)', '[2,3)', '[4,5)', '[6,7)', '[8,9)', '[10,11)', '[12,13)', '[14,15)', '[16,17)', '[18,19)', '[20,21)', '[22,23)', '[24,25)');

--
-- partitioned FK referencing inserts
--

INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[1,2)', datemultirange(daterange('2000-01-01', '2000-02-15')), '[1,2)'),
  ('[1,2)', datemultirange(daterange('2001-01-01', '2002-01-01')), '[2,3)'),
  ('[2,3)', datemultirange(daterange('2000-01-01', '2000-02-15')), '[1,2)');
-- should fail:
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[3,4)', datemultirange(daterange('2010-01-01', '2010-02-15')), '[1,2)');
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES
  ('[3,4)', datemultirange(daterange('2000-01-01', '2000-02-15')), '[3,4)');

--
-- partitioned FK referencing updates
--

UPDATE temporal_partitioned_fk_mltrng2mltrng SET valid_at = datemultirange(daterange('2000-01-01', '2000-02-13')) WHERE id = '[2,3)';
-- move a row from the first partition to the second
UPDATE temporal_partitioned_fk_mltrng2mltrng SET id = '[4,5)' WHERE id = '[1,2)';
-- move a row from the second partition to the first
UPDATE temporal_partitioned_fk_mltrng2mltrng SET id = '[1,2)' WHERE id = '[4,5)';
-- should fail:
UPDATE temporal_partitioned_fk_mltrng2mltrng SET valid_at = datemultirange(daterange('2000-01-01', '2000-04-01')) WHERE id = '[1,2)';

--
-- partitioned FK referenced updates NO ACTION
--

TRUNCATE temporal_partitioned_mltrng, temporal_partitioned_fk_mltrng2mltrng;
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2016-01-01', '2016-02-01')));
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2018-01-01', '2018-02-01')) WHERE id = '[5,6)';
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2016-02-01', '2016-03-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- should fail:
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- partitioned FK referenced deletes NO ACTION
--

TRUNCATE temporal_partitioned_mltrng, temporal_partitioned_fk_mltrng2mltrng;
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
DELETE FROM temporal_partitioned_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- should fail:
DELETE FROM temporal_partitioned_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- partitioned FK referenced updates RESTRICT
--

TRUNCATE temporal_partitioned_mltrng, temporal_partitioned_fk_mltrng2mltrng;
ALTER TABLE temporal_partitioned_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk;
ALTER TABLE temporal_partitioned_fk_mltrng2mltrng
  ADD CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk
  FOREIGN KEY (parent_id, PERIOD valid_at)
  REFERENCES temporal_partitioned_mltrng
  ON DELETE RESTRICT;
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2016-01-01', '2016-02-01')));
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2018-01-01', '2018-02-01')) WHERE id = '[5,6)';
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2016-02-01', '2016-03-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- should fail:
UPDATE temporal_partitioned_mltrng SET valid_at = datemultirange(daterange('2016-01-01', '2016-02-01'))
  WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- partitioned FK referenced deletes RESTRICT
--

TRUNCATE temporal_partitioned_mltrng, temporal_partitioned_fk_mltrng2mltrng;
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-01-01', '2018-02-01')));
INSERT INTO temporal_partitioned_mltrng (id, valid_at) VALUES ('[5,6)', datemultirange(daterange('2018-02-01', '2018-03-01')));
INSERT INTO temporal_partitioned_fk_mltrng2mltrng (id, valid_at, parent_id) VALUES ('[3,4)', datemultirange(daterange('2018-01-05', '2018-01-10')), '[5,6)');
DELETE FROM temporal_partitioned_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-02-01', '2018-03-01'));
-- should fail:
DELETE FROM temporal_partitioned_mltrng WHERE id = '[5,6)' AND valid_at = datemultirange(daterange('2018-01-01', '2018-02-01'));

--
-- partitioned FK referenced updates CASCADE
--

ALTER TABLE temporal_partitioned_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_mltrng
    ON DELETE CASCADE ON UPDATE CASCADE;

--
-- partitioned FK referenced deletes CASCADE
--

--
-- partitioned FK referenced updates SET NULL
--

ALTER TABLE temporal_partitioned_fk_mltrng2mltrng
  DROP CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_mltrng
    ON DELETE SET NULL ON UPDATE SET NULL;

--
-- partitioned FK referenced deletes SET NULL
--

--
-- partitioned FK referenced updates SET DEFAULT
--

ALTER TABLE temporal_partitioned_fk_mltrng2mltrng
  ALTER COLUMN parent_id SET DEFAULT '[0,1)',
  DROP CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk,
  ADD CONSTRAINT temporal_partitioned_fk_mltrng2mltrng_fk
    FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_partitioned_mltrng
    ON DELETE SET DEFAULT ON UPDATE SET DEFAULT;

--
-- partitioned FK referenced deletes SET DEFAULT
--

DROP TABLE temporal_partitioned_fk_mltrng2mltrng;
DROP TABLE temporal_partitioned_mltrng;

RESET datestyle;
