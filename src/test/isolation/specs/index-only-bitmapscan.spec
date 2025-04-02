# In the past we supported index-only bitmap heapscans. However the
# implementation was unsound, see
# https://postgr.es/m/873c33c5-ef9e-41f6-80b2-2f5e11869f1c%40garret.ru
#
# This test reliably triggered the problem before we removed the
# optimization. We keep the test around to make it less likely for a similar
# problem to be re-introduced.

setup
{
    -- by using a low fillfactor and a wide tuple we can get multiple blocks
    -- with just few rows
    CREATE TABLE ios_bitmap (a int NOT NULL, b int not null, pad char(1024) default '')
    WITH (AUTOVACUUM_ENABLED = false, FILLFACTOR = 10);

    INSERT INTO ios_bitmap SELECT g.i, g.i FROM generate_series(1, 10) g(i);

    CREATE INDEX ios_bitmap_a ON ios_bitmap(a);
    CREATE INDEX ios_bitmap_b ON ios_bitmap(b);
}

teardown
{
    DROP TABLE ios_bitmap;
}


session s1

setup
{
    SET enable_seqscan = false;
}

step s1_begin { BEGIN; }
step s1_commit { COMMIT; }


# The test query uses an or between two indexes to ensure make it more likely
# to use a bitmap index scan
#
# The row_number() hack is a way to have something returned (isolationtester
# doesn't display empty rows) while still allowing for the index-only scan
# optimization in bitmap heap scans, which requires an empty targetlist.
step s1_prepare
{
    DECLARE foo NO SCROLL CURSOR FOR SELECT row_number() OVER () FROM ios_bitmap WHERE a > 0 or b > 0;
}

step s1_explain
{
    EXPlAIN (COSTS OFF) DECLARE foo NO SCROLL CURSOR FOR SELECT row_number() OVER () FROM ios_bitmap WHERE a > 0 or b > 0;
}

step s1_fetch_1
{
    FETCH FROM foo;
}

step s1_fetch_all
{
    FETCH ALL FROM foo;
}


session s2

# Don't delete row 1 so we have a row for the cursor to "rest" on.
step s2_mod
{
  DELETE FROM ios_bitmap WHERE a > 1;
}

# Disable truncation, as otherwise we'll just wait for a timeout while trying
# to acquire the lock
step s2_vacuum
{
    VACUUM (TRUNCATE false) ios_bitmap;
}

permutation
  # Vacuum first, to ensure VM exists, otherwise the bitmapscan will consider
  # VM to be size 0, due to caching. Can't do that in setup because
  s2_vacuum

  # Delete nearly all rows, to make issue visible
  s2_mod

  # Verify that the appropriate plan is chosen
  s1_explain

  # Create a cursor
  s1_begin
  s1_prepare

  # Fetch one row from the cursor, that ensures the index scan portion is done
  # before the vacuum in the next step
  s1_fetch_1

  # With the bug this vacuum would have marked pages as all-visible that the
  # scan in the next step then would have considered all-visible, despite all
  # rows from those pages having been removed.
  s2_vacuum

  # If this returns any rows, the bug is present
  s1_fetch_all

  s1_commit
