# Test for vacuum's reduced processing of heap pages (used for any heap page
# where a cleanup lock isn't immediately available)
#
# Debugging tip: Change VACUUM to VACUUM VERBOSE to get feedback on what's
# really going on

# Use name type here to avoid TOAST table:
setup
{
  CREATE TABLE smalltbl AS SELECT i AS id, 't'::name AS t FROM generate_series(1,20) i;
  ALTER TABLE smalltbl SET (autovacuum_enabled = off);
  ALTER TABLE smalltbl ADD PRIMARY KEY (id);
}
setup
{
  VACUUM ANALYZE smalltbl;
}

teardown
{
  DROP TABLE smalltbl;
}

# This session holds a pin on smalltbl's only heap page:
session pinholder
step pinholder_cursor
{
  BEGIN;
  DECLARE c1 CURSOR FOR SELECT 1 AS dummy FROM smalltbl;
  FETCH NEXT FROM c1;
}
step pinholder_commit
{
  COMMIT;
}

# This session inserts and deletes tuples, potentially affecting reltuples:
session dml
step dml_insert
{
  INSERT INTO smalltbl SELECT max(id) + 1 FROM smalltbl;
}
step dml_delete
{
  DELETE FROM smalltbl WHERE id = (SELECT min(id) FROM smalltbl);
}
step dml_begin            { BEGIN; }
step dml_key_share        { SELECT id FROM smalltbl WHERE id = 3 FOR KEY SHARE; }
step dml_commit           { COMMIT; }

# Needed for Multixact test:
session dml_other
step dml_other_begin      { BEGIN; }
step dml_other_key_share  { SELECT id FROM smalltbl WHERE id = 3 FOR KEY SHARE; }
step dml_other_update     { UPDATE smalltbl SET t = 'u' WHERE id = 3; }
step dml_other_commit     { COMMIT; }

# This session runs non-aggressive VACUUM, but with maximally aggressive
# cutoffs for tuple freezing (e.g., FreezeLimit == OldestXmin):
session vacuumer
setup
{
  SET vacuum_freeze_min_age = 0;
  SET vacuum_multixact_freeze_min_age = 0;
}
step vacuumer_nonaggressive_vacuum
{
  VACUUM smalltbl;
}
step vacuumer_pg_class_stats
{
  SELECT relpages, reltuples FROM pg_class WHERE oid = 'smalltbl'::regclass;
}

# Test VACUUM's reltuples counting mechanism.
#
# Final pg_class.reltuples should never be affected by VACUUM's inability to
# get a cleanup lock on any page, except to the extent that any cleanup lock
# contention changes the number of tuples that remain ("missed dead" tuples
# are counted in reltuples, much like "recently dead" tuples).

# Easy case:
permutation
    vacuumer_pg_class_stats  # Start with 20 tuples
    dml_insert
    vacuumer_nonaggressive_vacuum
    vacuumer_pg_class_stats  # End with 21 tuples

# Harder case -- count 21 tuples at the end (like last time), but with cleanup
# lock contention this time:
permutation
    vacuumer_pg_class_stats  # Start with 20 tuples
    dml_insert
    pinholder_cursor
    vacuumer_nonaggressive_vacuum
    vacuumer_pg_class_stats  # End with 21 tuples
    pinholder_commit  # order doesn't matter

# Same as "harder case", but vary the order, and delete an inserted row:
permutation
    vacuumer_pg_class_stats  # Start with 20 tuples
    pinholder_cursor
    dml_insert
    dml_delete
    dml_insert
    vacuumer_nonaggressive_vacuum
    # reltuples is 21 here again -- "recently dead" tuple won't be included in
    # count here:
    vacuumer_pg_class_stats
    pinholder_commit  # order doesn't matter

# Same as "harder case", but initial insert and delete before cursor:
permutation
    vacuumer_pg_class_stats  # Start with 20 tuples
    dml_insert
    dml_delete
    pinholder_cursor
    dml_insert
    vacuumer_nonaggressive_vacuum
    # reltuples is 21 here again -- "missed dead" tuple ("recently dead" when
    # concurrent activity held back VACUUM's OldestXmin) won't be included in
    # count here:
    vacuumer_pg_class_stats
    pinholder_commit  # order doesn't matter

# Test VACUUM's mechanism for skipping MultiXact freezing.
#
# This provides test coverage for code paths that are only hit when we need to
# freeze, but inability to acquire a cleanup lock on a heap page makes
# freezing some XIDs/XMIDs < FreezeLimit/MultiXactCutoff impossible (without
# waiting for a cleanup lock, which non-aggressive VACUUM is unwilling to do).
permutation
    dml_begin
    dml_other_begin
    dml_key_share
    dml_other_key_share
    # Will get cleanup lock, can't advance relminmxid yet:
    # (though will usually advance relfrozenxid by ~2 XIDs)
    vacuumer_nonaggressive_vacuum
    pinholder_cursor
    dml_other_update
    dml_commit
    dml_other_commit
    # Can't cleanup lock, so still can't advance relminmxid here:
    # (relfrozenxid held back by XIDs in MultiXact too)
    vacuumer_nonaggressive_vacuum
    pinholder_commit
    # Pin was dropped, so will advance relminmxid, at long last:
    # (ditto for relfrozenxid advancement)
    vacuumer_nonaggressive_vacuum
