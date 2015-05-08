# INSERT...ON CONFLICT DO UPDATE test
#
# Other INSERT...ON CONFLICT DO UPDATE isolation tests illustrate the "MVCC
# violation" added to facilitate the feature, whereby a
# not-visible-to-our-snapshot tuple can be updated by our command all the same.
# This is generally needed to provide a guarantee of a successful INSERT or
# UPDATE in READ COMMITTED mode.  This MVCC violation is quite distinct from
# the putative "MVCC violation" that has existed in PostgreSQL for many years,
# the EvalPlanQual() mechanism, because that mechanism always starts from a
# tuple that is visible to the command's MVCC snapshot.  This test illustrates
# a slightly distinct user-visible consequence of the same MVCC violation
# generally associated with INSERT...ON CONFLICT DO UPDATE.  The impact of the
# MVCC violation goes a little beyond updating MVCC-invisible tuples.
#
# With INSERT...ON CONFLICT DO UPDATE, the UPDATE predicate is only evaluated
# once, on this conclusively-locked tuple, and not any other version of the
# same tuple.  It is therefore possible (in READ COMMITTED mode) that the
# predicate "fail to be satisfied" according to the command's MVCC snapshot.
# It might simply be that there is no row version visible, but it's also
# possible that there is some row version visible, but only as a version that
# doesn't satisfy the predicate.  If, however, the conclusively-locked version
# satisfies the predicate, that's good enough, and the tuple is updated.  The
# MVCC-snapshot-visible row version is denied the opportunity to prevent the
# UPDATE from taking place, because we don't walk the UPDATE chain in the usual
# way.

setup
{
  CREATE TABLE colors (key int4 PRIMARY KEY, color text, is_active boolean);
  INSERT INTO colors (key, color, is_active) VALUES(1, 'Red', false);
  INSERT INTO colors (key, color, is_active) VALUES(2, 'Green', false);
  INSERT INTO colors (key, color, is_active) VALUES(3, 'Blue', false);
}

teardown
{
  DROP TABLE colors;
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "insert1" {
    WITH t AS (
        INSERT INTO colors(key, color, is_active)
        VALUES(1, 'Brown', true), (2, 'Gray', true)
        ON CONFLICT (key) DO UPDATE
        SET color = EXCLUDED.color
        WHERE colors.is_active)
    SELECT * FROM colors ORDER BY key;}
step "select1surprise" { SELECT * FROM colors ORDER BY key; }
step "c1" { COMMIT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "update2" { UPDATE colors SET is_active = true WHERE key = 1; }
step "c2" { COMMIT; }

# Perhaps surprisingly, the session 1 MVCC-snapshot-visible tuple (the tuple
# with the pre-populated color 'Red') is denied the opportunity to prevent the
# UPDATE from taking place -- only the conclusively-locked tuple version
# matters, and so the tuple with key value 1 was updated to 'Brown' (but not
# tuple with key value 2, since nothing changed there):
permutation "update2" "insert1" "c2" "select1surprise" "c1"
