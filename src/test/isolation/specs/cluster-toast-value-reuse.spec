# Tests with CLUSTER for toast values

# This test does a relation rewrite, with toast values reused to make the
# rewrite cheaper (see data_todo = 0 case in toast_save_datum()).
#
# A first session updates the table with an attribute not toasted.  CLUSTER
# is then executed in a second session.  The comparison of the values
# allocated for the toasted values are done using a CTAS.  The allocated
# chunk_ids are saved before the rewrite, and compared after the rewrite.

# ---------- global setup ----------
setup
{
  DROP TABLE IF EXISTS cluster_toast_value CASCADE;
  DROP TABLE IF EXISTS cluster_chunk_id CASCADE;

  CREATE TABLE cluster_toast_value (
    id    serial PRIMARY KEY,
    flag  integer,
    value text);

  -- Make sure 'value' is large enough to be toasted.
  ALTER TABLE cluster_toast_value ALTER COLUMN value SET STORAGE EXTERNAL;

  -- Clustering index.
  CLUSTER cluster_toast_value_pkey ON cluster_toast_value;

  -- Seed data: one row with big string to force TOAST tuple and trigger the todo=0 code path.
  INSERT INTO cluster_toast_value(flag, value)
    VALUES (0, repeat(encode(sha256('1'), 'hex'), 120) || repeat('x', 8000));

  CLUSTER cluster_toast_value;
}

teardown
{
  DROP TABLE IF EXISTS cluster_toast_value;
  DROP TABLE IF EXISTS cluster_chunk_id;
}

session s1
step s1_begin  { BEGIN; }
step s1_update { UPDATE cluster_toast_value SET flag = 1 WHERE TRUE; }
step s1_commit { COMMIT; }

session s2
# Store the primary key values and their associated chunk IDs.  This makes
# sure that some data is captured.
step s2_store_chunk_ids {
  CREATE TABLE cluster_chunk_id AS
    SELECT c.id, pg_column_toast_chunk_id(c.value) AS chunk_id
    FROM cluster_toast_value c;
  SELECT count(*) FROM cluster_chunk_id;
}
step s2_cluster { CLUSTER cluster_toast_value; }

# Verify that toast values allocated are the same, indicating reuse.
# This query reports the tuples with toast values that do not match.
step s2_verify_chunk_ids {
  SELECT o.id AS chunk_ids_preserved
    FROM cluster_chunk_id o
    JOIN cluster_toast_value c ON o.id = c.id
    WHERE o.chunk_id != pg_column_toast_chunk_id(c.value);
}

# Run UPDATE with its transaction still open, then store the chunk IDs.
# CLUSTER will wait until the first transaction commit.  Finally, the chunk
# IDs are compared.
permutation s1_begin s1_update s2_store_chunk_ids s2_cluster s1_commit s2_verify_chunk_ids
