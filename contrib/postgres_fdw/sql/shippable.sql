-- ===================================================================
-- create FDW objects
-- ===================================================================

-- Error, extension isn't installed yet
ALTER SERVER loopback OPTIONS (ADD extensions 'cube');

-- Try again
CREATE EXTENSION cube;
ALTER SERVER loopback OPTIONS (ADD extensions 'cube');
ALTER SERVER loopback OPTIONS (DROP extensions);


-- ===================================================================
-- create objects used through FDW loopback server
-- ===================================================================

CREATE SCHEMA "SH 1";
CREATE TABLE "SH 1"."TBL 1" (
	"C 1" int NOT NULL,
	c2 int NOT NULL,
	c3 cube,
	c4 timestamptz
);

INSERT INTO "SH 1"."TBL 1"
	SELECT id,
	       2 * id,
	       cube(id,2*id),
	       '1970-01-01'::timestamptz + ((id % 100) || ' days')::interval
	FROM generate_series(1, 1000) id;

ANALYZE "SH 1"."TBL 1";

-- ===================================================================
-- create foreign table
-- ===================================================================

CREATE FOREIGN TABLE shft1 (
	"C 1" int NOT NULL,
	c2 int NOT NULL,
	c3 cube,
	c4 timestamptz
) SERVER loopback
OPTIONS (schema_name 'SH 1', table_name 'TBL 1');

-- ===================================================================
-- simple queries
-- ===================================================================

-- without operator shipping
EXPLAIN (COSTS false) SELECT * FROM shft1 LIMIT 1;
EXPLAIN VERBOSE SELECT c2 FROM shft1 WHERE c3 && cube(1.5,2.5);
SELECT c2 FROM shft1 WHERE c3 && cube(1.5,2.5);
EXPLAIN VERBOSE SELECT c2 FROM shft1 WHERE c3 && '(1.5),(2.5)'::cube;

-- with operator shipping
ALTER SERVER loopback OPTIONS (ADD extensions 'cube');
EXPLAIN VERBOSE SELECT c2 FROM shft1 WHERE c3 && cube(1.5,2.5);
SELECT c2 FROM shft1 WHERE c3 && cube(1.5,2.5);
EXPLAIN VERBOSE SELECT c2 FROM shft1 WHERE c3 && '(1.5),(2.5)'::cube;
EXPLAIN VERBOSE SELECT cube_dim(c3) FROM shft1 WHERE c3 && '(1.5),(2.5)'::cube;
SELECT cube_dim(c3) FROM shft1 WHERE c3 && '(1.5),(2.5)'::cube;

EXPLAIN VERBOSE SELECT c2 FROM shft1 WHERE cube_dim(c3) = 1 LIMIT 2;
SELECT c2 FROM shft1 WHERE cube_dim(c3) = 1 LIMIT 2;

-- ===================================================================
-- add a second server with different extension shipping
-- ===================================================================

DO $d$
    BEGIN
        EXECUTE $$CREATE SERVER loopback_two FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
    END;
$d$;

CREATE USER MAPPING FOR CURRENT_USER SERVER loopback_two;

CREATE EXTENSION seg;

CREATE TABLE seg_local (
	id integer,
	s seg,
  n text
);

INSERT INTO seg_local (id, s, n) VALUES (1, '1.0 .. 2.0', 'foo');
INSERT INTO seg_local (id, s, n) VALUES (2, '3.0 .. 4.0', 'bar');
INSERT INTO seg_local (id, s, n) VALUES (3, '5.0 .. 6.0', 'baz');

ANALYZE seg_local;

CREATE FOREIGN TABLE seg_remote_two (
  id integer,
  s seg,
  n text
) SERVER loopback_two
OPTIONS (table_name 'seg_local');

SELECT id FROM seg_local WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';
EXPLAIN VERBOSE SELECT id FROM seg_remote_two WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';
ALTER SERVER loopback_two OPTIONS (ADD extensions 'seg');
EXPLAIN VERBOSE SELECT id FROM seg_remote_two WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';

CREATE FOREIGN TABLE seg_remote_one (
  id integer,
  s seg,
  n text
) SERVER loopback
OPTIONS (table_name 'seg_local');

SELECT id FROM seg_remote_one WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';
EXPLAIN VERBOSE SELECT id FROM seg_remote_one WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';
EXPLAIN VERBOSE SELECT id FROM seg_remote_two WHERE s && '5.8 .. 6.2'::seg AND n = 'baz';


-- ===================================================================
-- clean up
-- ===================================================================
DROP FOREIGN TABLE seg_remote_one, seg_remote_two;
DROP USER MAPPING FOR CURRENT_USER SERVER loopback_two;
DROP SERVER loopback_two;
DROP TABLE seg_local;
DROP FOREIGN TABLE shft1;
DROP TABLE "SH 1"."TBL 1";
DROP SCHEMA "SH 1";
DROP EXTENSION cube;
DROP EXTENSION seg;
ALTER SERVER loopback OPTIONS (DROP extensions);
