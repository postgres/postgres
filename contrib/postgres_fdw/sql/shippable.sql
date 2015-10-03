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
-- clean up
-- ===================================================================

DROP FOREIGN TABLE shft1;
DROP TABLE "SH 1"."TBL 1";
DROP SCHEMA "SH 1";
DROP EXTENSION cube;
ALTER SERVER loopback OPTIONS (DROP extensions);
