--
-- Hot Standby tests
--
-- hs_primary_extremes.sql
--

drop table if exists hs_extreme;
create table hs_extreme (col1 integer);

CREATE OR REPLACE FUNCTION hs_subxids (n integer)
RETURNS void
LANGUAGE plpgsql
AS $$
    BEGIN
      IF n <= 0 THEN RETURN; END IF;
      INSERT INTO hs_extreme VALUES (n);
      PERFORM hs_subxids(n - 1);
      RETURN;
    EXCEPTION WHEN raise_exception THEN NULL; END;
$$;

BEGIN;
SELECT hs_subxids(257);
ROLLBACK;
BEGIN;
SELECT hs_subxids(257);
COMMIT;

set client_min_messages = 'warning';

CREATE OR REPLACE FUNCTION hs_locks_create (n integer)
RETURNS void
LANGUAGE plpgsql
AS $$
    BEGIN
      IF n <= 0 THEN
		CHECKPOINT;
		RETURN;
	  END IF;
      EXECUTE 'CREATE TABLE hs_locks_' || n::text || ' ()';
      PERFORM hs_locks_create(n - 1);
      RETURN;
    EXCEPTION WHEN raise_exception THEN NULL; END;
$$;

CREATE OR REPLACE FUNCTION hs_locks_drop (n integer)
RETURNS void
LANGUAGE plpgsql
AS $$
    BEGIN
      IF n <= 0 THEN
		CHECKPOINT;
		RETURN;
	  END IF;
	  EXECUTE 'DROP TABLE IF EXISTS hs_locks_' || n::text;
      PERFORM hs_locks_drop(n - 1);
      RETURN;
    EXCEPTION WHEN raise_exception THEN NULL; END;
$$;

BEGIN;
SELECT hs_locks_drop(257);
SELECT hs_locks_create(257);
SELECT count(*) > 257 FROM pg_locks;
ROLLBACK;
BEGIN;
SELECT hs_locks_drop(257);
SELECT hs_locks_create(257);
SELECT count(*) > 257 FROM pg_locks;
COMMIT;
SELECT hs_locks_drop(257);

SELECT pg_switch_wal();
