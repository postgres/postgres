# An inlined SQL function's targetlist uses NULL::int4 for dropped columns.
# Its whole-row Var retains the named composite type.  EPQ must be able to
# restore that datum into the subquery's targetlist-described scan slot.

setup
{
  CREATE TYPE epq_row AS (gone int, wide bigint, varlen text, id int, v int);
  ALTER TYPE epq_row DROP ATTRIBUTE gone;
  ALTER TYPE epq_row DROP ATTRIBUTE wide;
  ALTER TYPE epq_row DROP ATTRIBUTE varlen;
  CREATE TYPE epq_enum AS ENUM ('x');
  CREATE FUNCTION epq_cast(epq_enum) RETURNS int
    LANGUAGE plpgsql VOLATILE AS $$BEGIN RETURN 42; END$$;
  CREATE CAST (epq_enum AS int) WITH FUNCTION epq_cast(epq_enum) AS ASSIGNMENT;

  -- ORDER BY prevents coercion of v in place.  The volatile cast in the
  -- return-coercion projection then keeps that subquery from being pulled up.
  CREATE FUNCTION epq_function() RETURNS SETOF epq_row
    LANGUAGE sql STABLE AS
    $$SELECT 1 AS id, 'x'::epq_enum AS v ORDER BY 2$$;
  CREATE TABLE epq_lock (id int PRIMARY KEY, n int);
  INSERT INTO epq_lock VALUES (1, 0);
}

teardown
{
  DROP TABLE epq_lock;
  DROP FUNCTION epq_function();
  DROP CAST (epq_enum AS int);
  DROP FUNCTION epq_cast(epq_enum);
  DROP TYPE epq_enum, epq_row;
}

session s1
setup { SET default_transaction_isolation = 'read committed'; }
step begin1 { BEGIN; }
step update1 { UPDATE epq_lock SET n = n + 1 WHERE id = 1; }
step commit1 { COMMIT; }

session s2
setup { SET default_transaction_isolation = 'read committed'; }
step read2
{
  SELECT t.id, s.v
  FROM epq_lock t JOIN epq_function() s ON s.id = t.id
  FOR UPDATE OF t;
}
step explain2
{
  EXPLAIN (COSTS OFF)
  SELECT t.id, s.v
  FROM epq_lock t JOIN epq_function() s ON s.id = t.id
  FOR UPDATE OF t;
}

# Without a concurrent update, no EPQ reconstruction is needed.
permutation begin1 update1 commit1 read2

# EPQ must return the same row despite the dropped-column placeholders,
# including bigint and text attributes with different length, alignment,
# and pass-by-value metadata from int4.  The function is inlined as a
# subquery, which is not pulled up.
permutation explain2 begin1 update1 read2 commit1
