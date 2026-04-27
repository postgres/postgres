-- Tests for UPDATE/DELETE FOR PORTION OF

SET datestyle TO ISO, YMD;

-- Works on non-PK columns
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text NOT NULL
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2020-01-01)', 'one');

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = 'one^1';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-01-15' TO '2019-01-20';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- With a table alias with AS

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-01' TO '2019-02-03' AS t
  SET name = 'one^2';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-03' TO '2019-02-04' AS t;

-- With a table alias without AS

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-04' TO '2019-02-05' t
  SET name = 'one^3';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-05' TO '2019-02-06' t;

-- UPDATE with FROM

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-03-01' to '2019-03-02'
  SET name = 'one^4'
  FROM (SELECT '[1,2)'::int4range) AS t2(id)
  WHERE for_portion_of_test.id = t2.id;

-- DELETE with USING

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-03-02' TO '2019-03-03'
  USING (SELECT '[1,2)'::int4range) AS t2(id)
  WHERE for_portion_of_test.id = t2.id;

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- Works on more than one range
DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid1_at daterange,
  valid2_at daterange,
  name text NOT NULL
);
INSERT INTO for_portion_of_test (id, valid1_at, valid2_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', '[2015-01-01,2025-01-01)', 'one');

UPDATE for_portion_of_test
  FOR PORTION OF valid1_at FROM '2018-01-15' TO NULL
  SET name = 'foo';
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

UPDATE for_portion_of_test
  FOR PORTION OF valid2_at FROM '2018-01-15' TO NULL
  SET name = 'bar';
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid1_at FROM '2018-01-20' TO NULL;
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid2_at FROM '2018-01-20' TO NULL;
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

-- Test with NULLs in the scalar/range key columns.
-- This won't happen if there is a PRIMARY KEY or UNIQUE constraint
-- but FOR PORTION OF shouldn't require that.
DROP TABLE for_portion_of_test;
CREATE UNLOGGED TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', NULL, '1 null'),
  ('[1,2)', '(,)', '1 unbounded'),
  ('[1,2)', 'empty', '1 empty'),
  (NULL, NULL, NULL),
  (NULL, daterange('2018-01-01', '2019-01-01'), 'null key');
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'NULL to NULL';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

DROP TABLE for_portion_of_test;

--
-- UPDATE tests
--

CREATE TABLE for_portion_of_test (
  id int4range NOT NULL,
  valid_at daterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-01-05)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three'),
  ('[4,5)', '(,2018-04-01)', 'four'),
  ('[5,6)', '(,)', 'five')
  ;
\set QUIET false

-- Updating with a missing column fails
UPDATE for_portion_of_test
  FOR PORTION OF invalid_at FROM '2018-06-01' TO NULL
  SET name = 'foo'
  WHERE id = '[5,6)';

-- Updating the range fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  SET valid_at = '[1990-01-01,1999-01-01)'
  WHERE id = '[5,6)';

-- The wrong start type fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM 1 TO '2020-01-01'
  SET name = 'nope'
  WHERE id = '[3,4)';

-- The wrong end type fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2000-01-01' TO 4
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with timestamps reversed fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO '2018-01-01'
  SET name = 'three^1'
  WHERE id = '[3,4)';

-- Updating with a subquery fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM (SELECT '2018-01-01') TO '2018-06-01'
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with a column fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM lower(valid_at) TO NULL
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with timestamps equal does nothing
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-04-01'
  SET name = 'three^0'
  WHERE id = '[3,4)';

-- Updating a finite/open portion with a finite/open target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  SET name = 'three^1'
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Updating a finite/open portion with an open/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-03-01'
  SET name = 'three^2'
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Updating an open/finite portion with an open/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-02-01'
  SET name = 'four^1'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating an open/finite portion with a finite/open target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO NULL
  SET name = 'four^2'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating a finite/finite portion with an exact fit
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO '2018-02-01'
  SET name = 'four^3'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating an enclosed span
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'two^2'
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

-- Updating an open/open portion with a finite/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-01' TO '2019-01-01'
  SET name = 'five^1'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Updating an enclosed span with separate protruding spans
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO '2020-01-01'
  SET name = 'five^2'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Updating multiple enclosed spans
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'one^2'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (daterange('2018-03-10', '2018-03-15'))
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target, coerced from a string
UPDATE for_portion_of_test
  FOR PORTION OF valid_at ('[2018-03-15,2018-03-17)')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target of the wrong range subtype fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (int4range(1, 4))
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of a non-rangetype fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (4)
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of NULL fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (NULL)
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of empty does nothing
UPDATE for_portion_of_test
  FOR PORTION OF valid_at ('empty')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating the non-range part of the PK:
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-15' TO NULL
  SET id = '[6,7)'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id IN ('[1,2)', '[6,7)') ORDER BY id, valid_at;

-- UPDATE with no WHERE clause
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2030-01-01' TO NULL
  SET name = name || '*';

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;
\set QUIET true

-- Updating with a shift/reduce conflict
-- (requires a tsrange column)
CREATE UNLOGGED TABLE for_portion_of_test2 (
  id int4range,
  valid_at tsrange,
  name text
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', '[2000-01-01,2020-01-01)', 'one');
-- updates [2011-03-01 01:02:00, 2012-01-01) (note 2 minutes)
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM '2011-03-01'::timestamp + INTERVAL '1:02:03' HOUR TO MINUTE
    TO '2012-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';

-- TO is used for the bound but not the INTERVAL:
-- syntax error
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM '2013-03-01'::timestamp + INTERVAL '1:02:03' HOUR
    TO '2014-01-01'
  SET name = 'one^2'
  WHERE id = '[1,2)';

-- adding parens fixes it
-- updates [2015-03-01 01:00:00, 2016-01-01) (no minutes)
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM ('2015-03-01'::timestamp + INTERVAL '1:02:03' HOUR)
    TO '2016-01-01'
  SET name = 'one^3'
  WHERE id = '[1,2)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;
DROP TABLE for_portion_of_test2;

-- UPDATE FOR PORTION OF in a CTE:
-- The outer query sees the table how it was before the updates,
-- and with no leftovers yet,
-- but it also sees the new values via the RETURNING clause.
-- (We test RETURNING more directly, without a CTE, below.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
WITH update_apr AS (
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-05-01'
    SET name = 'Apr 2018'
    WHERE id = '[10,11)'
    RETURNING id, valid_at, name
)
SELECT *
  FROM for_portion_of_test AS t, update_apr
  WHERE t.id = update_apr.id;
SELECT * FROM for_portion_of_test WHERE id = '[10,11)' ORDER BY id, valid_at;

-- UPDATE FOR PORTION OF with current_date
-- (We take care not to make the expectation depend on the timestamp.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[99,100)', '[2000-01-01,)', 'foo');
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM current_date TO null
  SET name = 'bar'
  WHERE id = '[99,100)';
SELECT name, lower(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date - 1;
SELECT name, upper(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date + 1;

-- UPDATE FOR PORTION OF with clock_timestamp()
-- fails because the function is volatile:
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM clock_timestamp()::date TO null
  SET name = 'baz'
  WHERE id = '[99,100)';

-- clean up:
DELETE FROM for_portion_of_test WHERE id = '[99,100)';

-- Not visible to UPDATE:
-- Tuples updated/inserted within the CTE are not visible to the main query yet,
-- but neither are old tuples the CTE changed:
-- (This is the same behavior as without FOR PORTION OF.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[11,12)', '[2018-01-01,2020-01-01)', 'eleven');
WITH update_apr AS (
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-05-01'
    SET name = 'Apr 2018'
    WHERE id = '[11,12)'
    RETURNING id, valid_at, name
)
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  AS t
  SET name = 'May 2018'
  FROM update_apr AS j
  WHERE t.id = j.id;
SELECT * FROM for_portion_of_test WHERE id = '[11,12)' ORDER BY id, valid_at;
DELETE FROM for_portion_of_test WHERE id IN ('[10,11)', '[11,12)');

-- UPDATE FOR PORTION OF in a PL/pgSQL function
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
CREATE FUNCTION fpo_update(_id int4range, _target_from date, _target_til date)
RETURNS void LANGUAGE plpgsql AS
$$
BEGIN
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM $2 TO $3
    SET name = concat(_target_from::text, ' to ', _target_til::text)
    WHERE id = $1;
END;
$$;
SELECT fpo_update('[10,11)', '2015-01-01', '2019-01-01');
SELECT * FROM for_portion_of_test WHERE id = '[10,11)';

-- UPDATE FOR PORTION OF in a compiled SQL function
CREATE FUNCTION fpo_update()
RETURNS text
BEGIN ATOMIC
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
    SET name = 'one^1'
    RETURNING name;
END;
\sf+ fpo_update()
CREATE OR REPLACE function fpo_update()
RETURNS text
BEGIN ATOMIC
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at (daterange('2018-01-15', '2020-01-01') * daterange('2019-01-01', '2022-01-01'))
    SET name = 'one^1'
    RETURNING name;
END;
\sf+ fpo_update()
DROP FUNCTION fpo_update();

DROP TABLE for_portion_of_test;

--
-- DELETE tests
--

CREATE TABLE for_portion_of_test (
  id int4range NOT NULL,
  valid_at daterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-01-05)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three'),
  ('[4,5)', '(,2018-04-01)', 'four'),
  ('[5,6)', '(,)', 'five'),
  ('[6,7)', '[2018-01-01,)', 'six'),
  ('[7,8)', '(,2018-04-01)', 'seven'),
  ('[8,9)', '[2018-01-02,2018-02-03)', 'eight'),
  ('[8,9)', '[2018-02-03,2018-03-03)', 'eight'),
  ('[8,9)', '[2018-03-03,2018-04-04)', 'eight')
  ;
\set QUIET false

-- Deleting with a missing column fails
DELETE FROM for_portion_of_test
  FOR PORTION OF invalid_at FROM '2018-06-01' TO NULL
  WHERE id = '[5,6)';

-- The wrong start type fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM 1 TO '2020-01-01'
  WHERE id = '[3,4)';

-- The wrong end type fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2000-01-01' TO 4
  WHERE id = '[3,4)';

-- Deleting with timestamps reversed fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO '2018-01-01'
  WHERE id = '[3,4)';

-- Deleting with a subquery fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM (SELECT '2018-01-01') TO '2018-06-01'
  WHERE id = '[3,4)';

-- Deleting with a column fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM lower(valid_at) TO NULL
  WHERE id = '[3,4)';

-- Deleting with timestamps equal does nothing
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-04-01'
  WHERE id = '[3,4)';

-- Deleting a finite/open portion with a finite/open target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Deleting a finite/open portion with an open/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-03-01'
  WHERE id = '[6,7)';
SELECT * FROM for_portion_of_test WHERE id = '[6,7)' ORDER BY id, valid_at;

-- Deleting an open/finite portion with an open/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-02-01'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Deleting an open/finite portion with a finite/open target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO NULL
  WHERE id = '[7,8)';
SELECT * FROM for_portion_of_test WHERE id = '[7,8)' ORDER BY id, valid_at;

-- Deleting a finite/finite portion with an exact fit
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-04-01'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Deleting an enclosed span
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

-- Deleting an open/open portion with a finite/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-01' TO '2019-01-01'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Deleting an enclosed span with separate protruding spans
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-03' TO '2018-03-03'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting multiple enclosed spans
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  WHERE id = '[8,9)';
SELECT * FROM for_portion_of_test WHERE id = '[8,9)' ORDER BY id, valid_at;

-- Deleting with a direct target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (daterange('2018-03-10', '2018-03-15'))
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting with a direct target, coerced from a string
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at ('[2018-03-15,2018-03-17)')
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting with a direct target of the wrong range subtype fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (int4range(1, 4))
  WHERE id = '[1,2)';

-- Deleting with a direct target of a non-rangetype fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (4)
  WHERE id = '[1,2)';

-- Deleting with a direct target of NULL fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (NULL)
  WHERE id = '[1,2)';

-- Deleting with a direct target of empty does nothing
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at ('empty')
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- DELETE with no WHERE clause
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2030-01-01' TO NULL;

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;
\set QUIET true

-- UPDATE ... RETURNING returns only the updated values
-- (not the inserted side values, which are added by a separate "statement"):
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-02-15'
  SET name = 'three^3'
  WHERE id = '[3,4)'
  RETURNING *;

-- UPDATE ... RETURNING supports NEW and OLD valid_at
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-10' TO '2018-02-20'
  SET name = 'three^4'
  WHERE id = '[3,4)'
  RETURNING OLD.name, NEW.name, OLD.valid_at, NEW.valid_at;

-- DELETE FOR PORTION OF with current_date
-- (We take care not to make the expectation depend on the timestamp.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[99,100)', '[2000-01-01,)', 'foo');
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM current_date TO null
  WHERE id = '[99,100)';
SELECT name, lower(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date - 1;
SELECT name, upper(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date + 1;

-- DELETE FOR PORTION OF with clock_timestamp()
-- fails because the function is volatile:
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM clock_timestamp()::date TO null
  WHERE id = '[99,100)';

-- clean up:
DELETE FROM for_portion_of_test WHERE id = '[99,100)';

-- DELETE ... RETURNING returns the deleted values, regardless of bounds
-- (not the inserted side values, which are added by a separate "statement"):
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-02' TO '2018-02-03'
  WHERE id = '[3,4)'
  RETURNING *;

-- DELETE FOR PORTION OF in a PL/pgSQL function
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
CREATE FUNCTION fpo_delete(_id int4range, _target_from date, _target_til date)
RETURNS void LANGUAGE plpgsql AS
$$
BEGIN
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at FROM $2 TO $3
    WHERE id = $1;
END;
$$;
SELECT fpo_delete('[10,11)', '2015-01-01', '2019-01-01');
SELECT * FROM for_portion_of_test WHERE id = '[10,11)';
DELETE FROM for_portion_of_test WHERE id IN ('[10,11)');

-- DELETE FOR PORTION OF in a compiled SQL function
CREATE FUNCTION fpo_delete()
RETURNS text
BEGIN ATOMIC
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
    RETURNING name;
END;
\sf+ fpo_delete()
CREATE OR REPLACE function fpo_delete()
RETURNS text
BEGIN ATOMIC
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at (daterange('2018-01-15', '2020-01-01') * daterange('2019-01-01', '2022-01-01'))
    RETURNING name;
END;
\sf+ fpo_delete()
DROP FUNCTION fpo_delete();


-- test domains and CHECK constraints

-- With a domain on a rangetype
CREATE DOMAIN daterange_d AS daterange CHECK (upper(VALUE) <> '2005-05-05'::date);
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at daterange_d,
  name text
);
INSERT INTO for_portion_of_test2 VALUES
  (1, '[2000-01-01,2020-01-01)', 'one'),
  (2, '[2000-01-01,2020-01-01)', 'two');
INSERT INTO for_portion_of_test2 VALUES
  (1, '[2000-01-01,2005-05-05)', 'nope');
-- UPDATE works:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2010-01-01' TO '2010-01-05'
  SET name = 'one^1'
  WHERE id = 1;
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('[2010-01-07,2010-01-09)')
  SET name = 'one^2'
  WHERE id = 1;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- The target is allowed to violate the domain:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  SET name = 'miss'
  WHERE id = -1;
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('[1999-01-01,2005-05-05)')
  SET name = 'miss'
  WHERE id = -1;
-- test the updated row violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  SET name = 'one^3'
  WHERE id = 1;
-- test inserts violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2005-05-05' TO '2010-01-01'
  SET name = 'one^3'
  WHERE id = 1;
-- test updated row violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (upper(valid_at) <> '2001-01-11');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2001-01-11'
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (lower(valid_at) <> '2002-02-02');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-02-02'
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- DELETE works:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2010-01-01' TO '2010-01-05'
  WHERE id = 2;
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('[2010-01-07,2010-01-09)')
  WHERE id = 2;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
-- The target is allowed to violate the domain:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  WHERE id = -1;
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('[1999-01-01,2005-05-05)')
  WHERE id = -1;
-- test inserts violating the domain
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2005-05-05' TO '2010-01-01'
  WHERE id = 2;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (lower(valid_at) <> '2002-02-02');
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-02-02'
  WHERE id = 2;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
DROP TABLE for_portion_of_test2;

-- With a domain on a multirangetype
CREATE FUNCTION multirange_lowers(mr anymultirange) RETURNS anyarray LANGUAGE sql AS $$
  SELECT array_agg(lower(r)) FROM UNNEST(mr) u(r);
$$;
CREATE FUNCTION multirange_uppers(mr anymultirange) RETURNS anyarray LANGUAGE sql AS $$
  SELECT array_agg(upper(r)) FROM UNNEST(mr) u(r);
$$;
CREATE DOMAIN datemultirange_d AS datemultirange CHECK (NOT '2005-05-05'::date = ANY (multirange_uppers(VALUE)));
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at datemultirange_d,
  name text
);
INSERT INTO for_portion_of_test2 VALUES
  (1, '{[2000-01-01,2020-01-01)}', 'one'),
  (2, '{[2000-01-01,2020-01-01)}', 'two');
INSERT INTO for_portion_of_test2 VALUES
  (1, '{[2000-01-01,2005-05-05)}', 'nope');
-- UPDATE works:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2010-01-07,2010-01-09)}')
  SET name = 'one^2'
  WHERE id = 1;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- The target is allowed to violate the domain:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  SET name = 'miss'
  WHERE id = -1;
-- test the updated row violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  SET name = 'one^3'
  WHERE id = 1;
-- test inserts violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2005-05-05,2010-01-01)}')
  SET name = 'one^3'
  WHERE id = 1;
-- test updated row violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (upper(valid_at) <> '2001-01-11');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2000-01-01,2001-01-11)}')
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (NOT '2002-02-02'::date = ANY (multirange_lowers(valid_at)));
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2001-01-01,2002-02-02)}')
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- DELETE works:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2010-01-07,2010-01-09)}')
  WHERE id = 2;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
-- The target is allowed to violate the domain:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  WHERE id = -1;
-- test inserts violating the domain
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2005-05-05,2010-01-01)}')
  WHERE id = 2;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (NOT '2002-02-02'::date = ANY (multirange_lowers(valid_at)));
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2001-01-01,2002-02-02)}')
  WHERE id = 2;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
DROP TABLE for_portion_of_test2;

-- test on non-range/multirange columns

-- With a direct target and a scalar column
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at date,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '2020-01-01', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('2010-01-01')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('2010-01-01');
DROP TABLE for_portion_of_test2;

-- With a direct target and a non-{,multi}range gistable column without overlaps
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at point,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '0,0', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('1,1')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('1,1');
DROP TABLE for_portion_of_test2;

-- With a direct target and a non-{,multi}range column with overlaps
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at box,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '0,0,4,4', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('1,1,2,2')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('1,1,2,2');
DROP TABLE for_portion_of_test2;

-- test that we run triggers on the UPDATE/DELETEd row and the INSERTed rows

CREATE FUNCTION dump_trigger()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE '%: % % %:',
    TG_NAME, TG_WHEN, TG_OP, TG_LEVEL;

  IF TG_ARGV[0] THEN
    RAISE NOTICE '  old: %', (SELECT string_agg(old_table::text, '\n       ') FROM old_table);
  ELSE
    RAISE NOTICE '  old: %', OLD.valid_at;
  END IF;
  IF TG_ARGV[1] THEN
    RAISE NOTICE '  new: %', (SELECT string_agg(new_table::text, '\n       ') FROM new_table);
  ELSE
    RAISE NOTICE '  new: %', NEW.valid_at;
  END IF;

  IF TG_OP = 'INSERT' OR TG_OP = 'UPDATE' THEN
    RETURN NEW;
  ELSIF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
END;
$$;

-- statement triggers:

CREATE TRIGGER fpo_before_stmt
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_stmt
  AFTER INSERT ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_update_stmt
  AFTER UPDATE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_delete_stmt
  AFTER DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

-- row triggers:

CREATE TRIGGER fpo_before_row
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);


UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2021-01-01' TO '2022-01-01'
  SET name = 'five^3'
  WHERE id = '[5,6)';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2023-01-01' TO '2024-01-01'
  WHERE id = '[5,6)';

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- Triggers with a custom transition table name:

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test VALUES ('[1,2)', '[2018-01-01,2020-01-01)', 'one');

-- statement triggers:

CREATE TRIGGER fpo_before_stmt
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_stmt
  AFTER INSERT ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, true);

CREATE TRIGGER fpo_after_update_stmt
  AFTER UPDATE ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(true, true);

CREATE TRIGGER fpo_after_delete_stmt
  AFTER DELETE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(true, false);

-- row triggers:

CREATE TRIGGER fpo_before_row
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, true);

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(true, true);

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(true, false);

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = '2018-01-15_to_2019-01-01';
ROLLBACK;

BEGIN;
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-21';
ROLLBACK;

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-02'
  SET name = 'NULL_to_2018-01-01';
ROLLBACK;

-- Deferred triggers
-- (must be CONSTRAINT triggers thus AFTER ROW with no transition tables)

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-01,2020-01-01)', 'one');

CREATE CONSTRAINT TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE CONSTRAINT TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE CONSTRAINT TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = '2018-01-15_to_2019-01-01';
COMMIT;

BEGIN;
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-21';
COMMIT;

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-02'
  SET name = 'NULL_to_2018-01-01';
COMMIT;

SELECT * FROM for_portion_of_test;

-- test FOR PORTION OF from triggers during FOR PORTION OF:

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-01,2020-01-01)', 'one'),
  ('[2,3)', '[2018-01-01,2020-01-01)', 'two'),
  ('[3,4)', '[2018-01-01,2020-01-01)', 'three'),
  ('[4,5)', '[2018-01-01,2020-01-01)', 'four');

CREATE FUNCTION trg_fpo_update()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() = 1 THEN
    UPDATE for_portion_of_test
      FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-03-01'
      SET name = CONCAT(name, '^')
      WHERE id = OLD.id;
  END IF;
  RETURN CASE WHEN 'TG_OP' = 'DELETE' THEN OLD ELSE NEW END;
END;
$$;

CREATE FUNCTION trg_fpo_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() = 1 THEN
    DELETE FROM for_portion_of_test
      FOR PORTION OF valid_at FROM '2018-03-01' TO '2018-04-01'
      WHERE id = OLD.id;
  END IF;
  RETURN CASE WHEN 'TG_OP' = 'DELETE' THEN OLD ELSE NEW END;
END;
$$;

-- UPDATE FOR PORTION OF from a trigger fired by UPDATE FOR PORTION OF

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_update();

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '*')
  WHERE id = '[1,2)';

SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_update_row ON for_portion_of_test;

-- UPDATE FOR PORTION OF from a trigger fired by DELETE FOR PORTION OF

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_update();

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_delete_row ON for_portion_of_test;

-- DELETE FOR PORTION OF from a trigger fired by UPDATE FOR PORTION OF

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_delete();

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '*')
  WHERE id = '[3,4)';

SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_update_row ON for_portion_of_test;

-- DELETE FOR PORTION OF from a trigger fired by DELETE FOR PORTION OF

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_delete();

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  WHERE id = '[4,5)';

SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_delete_row ON for_portion_of_test;

-- Test with multiranges

CREATE TABLE for_portion_of_test2 (
  id int4range NOT NULL,
  valid_at datemultirange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_test2_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03)'), daterange('2018-02-04', '2018-03-03')), 'one'),
  ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04)')), 'one'),
  ('[2,3)', datemultirange(daterange('2018-01-01', '2018-05-01)')), 'two'),
  ('[3,4)', datemultirange(daterange('2018-01-01', null)), 'three');
  ;

-- Updating with FROM/TO
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2010-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';
-- Updating with multirange
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (datemultirange(daterange('2018-01-10', '2018-02-10'), daterange('2018-03-05', '2018-05-01')))
  SET name = 'one^1'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;
-- Updating with string coercion
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2018-03-05,2018-03-10)}')
  SET name = 'one^2'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;
-- Updating with the wrong range subtype fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1,4)}'::int4multirange)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with a non-multirangetype fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (4)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with NULL fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (NULL)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with empty does nothing
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{}')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;

-- Deleting with FROM/TO
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2010-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';
-- Deleting with multirange
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (datemultirange(daterange('2018-01-15', '2018-02-15'), daterange('2018-03-01', '2018-03-15')))
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test2 WHERE id = '[2,3)' ORDER BY valid_at;
-- Deleting with string coercion
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2018-03-05,2018-03-20)}')
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test2 WHERE id = '[2,3)' ORDER BY valid_at;
-- Deleting with the wrong range subtype fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[1,4)}'::int4multirange)
  WHERE id = '[2,3)';
-- Deleting with a non-multirangetype fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (4)
  WHERE id = '[2,3)';
-- Deleting with NULL fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (NULL)
  WHERE id = '[2,3)';
-- Deleting with empty does nothing
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{}')
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;

DROP TABLE for_portion_of_test2;

-- Test with a custom range type

CREATE TYPE mydaterange AS range(subtype=date);

CREATE TABLE for_portion_of_test2 (
  id int4range NOT NULL,
  valid_at mydaterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_test2_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-05-01)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three');
  ;

UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2018-01-10' TO '2018-02-10'
  SET name = 'one^1'
  WHERE id = '[1,2)';

DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2018-02-15'
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;

DROP TABLE for_portion_of_test2;
DROP TYPE mydaterange;

-- Test FOR PORTION OF against a partitioned table.
-- temporal_partitioned_1 has the same attnums as the root
-- temporal_partitioned_3 has the different attnums from the root
-- temporal_partitioned_5 has the different attnums too, but reversed

CREATE TABLE temporal_partitioned (
  id int4range,
  valid_at daterange,
  name text,
  CONSTRAINT temporal_paritioned_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE temporal_partitioned_1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,2)', '[2,3)');
CREATE TABLE temporal_partitioned_3 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,4)', '[4,5)');
CREATE TABLE temporal_partitioned_5 PARTITION OF temporal_partitioned FOR VALUES IN ('[5,6)', '[6,7)');

ALTER TABLE temporal_partitioned DETACH PARTITION temporal_partitioned_3;
ALTER TABLE temporal_partitioned_3 DROP COLUMN id, DROP COLUMN valid_at;
ALTER TABLE temporal_partitioned_3 ADD COLUMN id int4range NOT NULL, ADD COLUMN valid_at daterange NOT NULL;
ALTER TABLE temporal_partitioned ATTACH PARTITION temporal_partitioned_3 FOR VALUES IN ('[3,4)', '[4,5)');

ALTER TABLE temporal_partitioned DETACH PARTITION temporal_partitioned_5;
ALTER TABLE temporal_partitioned_5 DROP COLUMN id, DROP COLUMN valid_at;
ALTER TABLE temporal_partitioned_5 ADD COLUMN valid_at daterange NOT NULL, ADD COLUMN id int4range NOT NULL;
ALTER TABLE temporal_partitioned ATTACH PARTITION temporal_partitioned_5 FOR VALUES IN ('[5,6)', '[6,7)');

INSERT INTO temporal_partitioned (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one'),
  ('[3,4)', daterange('2000-01-01', '2010-01-01'), 'three'),
  ('[5,6)', daterange('2000-01-01', '2010-01-01'), 'five');

SELECT * FROM temporal_partitioned;

-- Update without moving within partition 1
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';

-- Update without moving within partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'three^1'
  WHERE id = '[3,4)';

-- Update without moving within partition 5
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'five^1'
  WHERE id = '[5,6)';

-- Move from partition 1 to partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'one^2',
      id = '[4,5)'
  WHERE id = '[1,2)';

-- Move from partition 3 to partition 1
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'three^2',
      id = '[2,3)'
  WHERE id = '[3,4)';

-- Move from partition 5 to partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'five^2',
      id = '[3,4)'
  WHERE id = '[5,6)';

-- Update all partitions at once (each with leftovers)

SELECT * FROM temporal_partitioned ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_1 ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_3 ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_5 ORDER BY id, valid_at;

DROP TABLE temporal_partitioned;

-- UPDATE/DELETE FOR PORTION OF with RULEs
CREATE TABLE fpo_rule (f1 bigint, f2 int4range);
INSERT INTO fpo_rule VALUES (1, '[1, 11)');

CREATE RULE fpo_rule1 AS ON INSERT TO fpo_rule
  DO INSTEAD UPDATE fpo_rule FOR PORTION OF f2 FROM 1 TO 4 SET f1 = 2;
INSERT INTO fpo_rule VALUES (1, '[1, 11)');
SELECT * FROM fpo_rule ORDER BY f1;

CREATE RULE fpo_rule2 AS ON INSERT TO fpo_rule
  DO INSTEAD DELETE FROM fpo_rule FOR PORTION OF f2 FROM 1 TO 4;
INSERT INTO fpo_rule VALUES (1, '[1, 11)');
SELECT * FROM fpo_rule ORDER BY f1;

CREATE RULE fpo_rule3 AS ON DELETE TO fpo_rule
  DO INSTEAD UPDATE fpo_rule FOR PORTION OF f2 FROM 1 TO 8 SET f1 = 2;
DELETE FROM fpo_rule FOR PORTION OF f2 FROM 1 TO 5;
SELECT * FROM fpo_rule ORDER BY f1;

DROP RULE fpo_rule3 ON fpo_rule;
CREATE RULE fpo_rule4 AS ON UPDATE TO fpo_rule
  DO INSTEAD DELETE FROM fpo_rule FOR PORTION OF f2 FROM 6 TO 9;
UPDATE fpo_rule FOR PORTION OF f2 FROM 4 TO 9 SET f1 = 12;
SELECT * FROM fpo_rule ORDER BY f1;

DROP RULE fpo_rule4 ON fpo_rule;
CREATE RULE fpo_rule5 AS ON UPDATE TO fpo_rule
  DO ALSO DELETE FROM fpo_rule FOR PORTION OF f2 FROM 4 TO 6;
UPDATE fpo_rule FOR PORTION OF f2 FROM 9 TO 10 SET f1 = 3;
SELECT * FROM fpo_rule ORDER BY f1;

DROP TABLE fpo_rule;

RESET datestyle;
