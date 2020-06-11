# Test for interactions between SSI's "conflict out" handling for heapam and
# concurrently updated tuple
#
# See bug report:
# https://postgr.es/m/db7b729d-0226-d162-a126-8a8ab2dc4443%40jepsen.io

setup
{
  CREATE TABLE txn0(id int4 PRIMARY KEY, val TEXT);
  CREATE TABLE txn1(id int4 PRIMARY KEY, val TEXT);
}

teardown
{
  DROP TABLE txn0;
  DROP TABLE txn1;
}

session "foo"
setup                  { BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "foo_select"      { SELECT * FROM txn0 WHERE id = 42; }
step "foo_insert"      { INSERT INTO txn1 SELECT 7, 'foo_insert'; }
step "foo_commit"      { COMMIT; }

session "bar"
setup                  { BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "bar_select"      { SELECT * FROM txn1 WHERE id = 7; }
step "bar_insert"      { INSERT INTO txn0 SELECT 42, 'bar_insert'; }
step "bar_commit"      { COMMIT; }

# This session creates the conditions that confused bar's "conflict out"
# handling in old releases affected by bug:
session "trouble"
setup                  { BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "trouble_update"  { UPDATE txn1 SET val = 'add physical version for "bar_select"' WHERE id = 7; }
step "trouble_delete"  { DELETE FROM txn1 WHERE id = 7; }
step "trouble_abort"   { ABORT; }

permutation "foo_select"
    "bar_insert"
    "foo_insert" "foo_commit"
    "trouble_update"   # Updates tuple...
    "bar_select"       # Should observe one distinct XID per version
    "bar_commit"       # "bar" should fail here at the latest
    "trouble_abort"

# Same as above, but "trouble" session DELETEs this time around
permutation "foo_select"
    "bar_insert"
    "foo_insert" "foo_commit"
    "trouble_delete"   # Deletes tuple...
    "bar_select"       # Should observe foo's XID
    "bar_commit"       # "bar" should fail here at the latest
    "trouble_abort"
