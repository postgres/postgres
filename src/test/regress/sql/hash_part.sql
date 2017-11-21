--
-- Hash partitioning.
--

CREATE OR REPLACE FUNCTION hashint4_noop(int4, int8) RETURNS int8 AS
$$SELECT coalesce($1,0)::int8$$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR CLASS test_int4_ops FOR TYPE int4 USING HASH AS
OPERATOR 1 = , FUNCTION 2 hashint4_noop(int4, int8);

CREATE OR REPLACE FUNCTION hashtext_length(text, int8) RETURNS int8 AS
$$SELECT length(coalesce($1,''))::int8$$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR CLASS test_text_ops FOR TYPE text USING HASH AS
OPERATOR 1 = , FUNCTION 2 hashtext_length(text, int8);

CREATE TABLE mchash (a int, b text, c jsonb)
  PARTITION BY HASH (a test_int4_ops, b test_text_ops);
CREATE TABLE mchash1
  PARTITION OF mchash FOR VALUES WITH (MODULUS 4, REMAINDER 0);

-- invalid OID, no such table
SELECT satisfies_hash_partition(0, 4, 0, NULL);

-- not partitioned
SELECT satisfies_hash_partition('tenk1'::regclass, 4, 0, NULL);

-- partition rather than the parent
SELECT satisfies_hash_partition('mchash1'::regclass, 4, 0, NULL);

-- invalid modulus
SELECT satisfies_hash_partition('mchash'::regclass, 0, 0, NULL);

-- remainder too small
SELECT satisfies_hash_partition('mchash'::regclass, 1, -1, NULL);

-- remainder too large
SELECT satisfies_hash_partition('mchash'::regclass, 1, 1, NULL);

-- modulus is null
SELECT satisfies_hash_partition('mchash'::regclass, NULL, 0, NULL);

-- remainder is null
SELECT satisfies_hash_partition('mchash'::regclass, 4, NULL, NULL);

-- too many arguments
SELECT satisfies_hash_partition('mchash'::regclass, 4, 0, NULL::int, NULL::text, NULL::json);

-- too few arguments
SELECT satisfies_hash_partition('mchash'::regclass, 3, 1, NULL::int);

-- wrong argument type
SELECT satisfies_hash_partition('mchash'::regclass, 2, 1, NULL::int, NULL::int);

-- ok, should be false
SELECT satisfies_hash_partition('mchash'::regclass, 4, 0, 0, ''::text);

-- ok, should be true
SELECT satisfies_hash_partition('mchash'::regclass, 4, 0, 1, ''::text);

-- argument via variadic syntax, should fail because not all partitioning
-- columns are of the correct type
SELECT satisfies_hash_partition('mchash'::regclass, 2, 1,
								variadic array[1,2]::int[]);

-- multiple partitioning columns of the same type
CREATE TABLE mcinthash (a int, b int, c jsonb)
  PARTITION BY HASH (a test_int4_ops, b test_int4_ops);

-- now variadic should work, should be false
SELECT satisfies_hash_partition('mcinthash'::regclass, 4, 0,
								variadic array[0, 0]);

-- should be true
SELECT satisfies_hash_partition('mcinthash'::regclass, 4, 0,
								variadic array[1, 0]);

-- wrong length
SELECT satisfies_hash_partition('mcinthash'::regclass, 4, 0,
								variadic array[]::int[]);

-- wrong type
SELECT satisfies_hash_partition('mcinthash'::regclass, 4, 0,
								variadic array[now(), now()]);

-- cleanup
DROP TABLE mchash;
DROP TABLE mcinthash;
DROP OPERATOR CLASS test_text_ops USING hash;
DROP OPERATOR CLASS test_int4_ops USING hash;
DROP FUNCTION hashint4_noop(int4, int8);
DROP FUNCTION hashtext_length(text, int8);
