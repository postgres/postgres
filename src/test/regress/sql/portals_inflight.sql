--
-- PORTALS_INFLIGHT
--
-- Checks for portals manipulated during initial setup.
--

-- Limit noise with CONTEXT and such.
\set VERBOSITY terse

CREATE TABLE cl_p (a int) PARTITION BY RANGE (a);
CREATE TABLE cl_p1 PARTITION OF cl_p FOR VALUES FROM (0) TO (10);
CREATE TABLE cl_p2 PARTITION OF cl_p FOR VALUES FROM (10) TO (20);
INSERT INTO cl_p SELECT generate_series(0, 19);

-- Run a statement.  If "late" is true, run it only once the portal is visible
-- in pg_cursors; it would not be the case when planning.  Cursor "c" is
-- used for the SPI tests, and psql uses the unnamed portal for some extended
-- protocol tests.
CREATE FUNCTION cl_run(stmt text, late bool) RETURNS int
LANGUAGE plpgsql VOLATILE AS
$$ BEGIN
  IF NOT late OR
      EXISTS (SELECT 1 FROM pg_cursors WHERE name IN ('c', '')) THEN
    EXECUTE stmt;
  END IF;
  RETURN 1;
END $$;
-- Stable function calling volatile function. That can be evaluated at planning.
CREATE FUNCTION cl_stab(stmt text, late bool) RETURNS int
LANGUAGE sql STABLE AS
$$ SELECT cl_run(stmt, late) $$;

-- SPI, executor startup.
BEGIN;
DO $$
DECLARE r refcursor := 'c';
BEGIN
  OPEN r FOR SELECT * FROM cl_p WHERE a < cl_stab('CLOSE c', true) + 5;
END $$;
ROLLBACK;
BEGIN;
DO $$
DECLARE r refcursor := 'c';
  x int;
BEGIN
  OPEN r FOR SELECT * FROM cl_p WHERE a < cl_stab('CLOSE ALL', true) + 5;
  FETCH r INTO x;
  RAISE NOTICE 'fetched %', x;
END $$;
ROLLBACK;

-- SPI, planning.
BEGIN;
DO $$
DECLARE r refcursor := 'c';
BEGIN
  OPEN r FOR SELECT * FROM cl_p WHERE a < cl_stab('CLOSE c', false) + 5;
END $$;
ROLLBACK;
BEGIN;
DO $$
DECLARE r refcursor := 'c';
  x int;
BEGIN
  OPEN r FOR SELECT * FROM cl_p WHERE a < cl_stab('CLOSE ALL', false) + 5;
  FETCH r INTO x;
  RAISE NOTICE 'fetched %', x;
END $$;
ROLLBACK;

-- SPI, failure after the portal is defined before being started.
CREATE FUNCTION cl_ro_open() RETURNS int
LANGUAGE plpgsql STABLE AS
$$ DECLARE r refcursor := 'c';
BEGIN
  OPEN r FOR SELECT * FROM cl_p FOR UPDATE;
  RETURN 1;
END $$;
BEGIN;
SELECT cl_ro_open();
ROLLBACK;
SELECT count(*) FROM pg_cursors;
BEGIN;
SAVEPOINT s;
SELECT cl_ro_open();
ROLLBACK TO s;
SELECT count(*) FROM pg_cursors;
COMMIT;

-- Extended protocol.
SELECT * FROM cl_p WHERE a < cl_stab('CLOSE ALL', true) + $1 \bind 5 \g
SELECT * FROM cl_p WHERE a < cl_stab('CLOSE ALL', false) + $1 \bind 5 \g

-- Cleanup
\set VERBOSITY default
DROP TABLE cl_p;
DROP FUNCTION cl_ro_open();
DROP FUNCTION cl_stab(text, bool);
DROP FUNCTION cl_run(text, bool);
