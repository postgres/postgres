# Backwards scan isolation test
#
# Backwards scans cannot unreservedly trust their saved left link: by the time
# the scan follows it, concurrent page splits and/or page deletions may have
# left it pointing to a page that is no longer the correct page for the scan
# to read next.  The scan checks for this by verifying that the pointed-to
# page's right link still points back to the page that the scan just read, and
# recovers when it doesn't (see nbtree/README for details).
#
# Each permutation makes the scan wait "between pages" at the nbtree-walk-left
# injection point while the concurrent session splits and/or deletes pages,
# then wakes it, forcing the scan to take one of its recovery paths.  The
# notice-mode injection points confirm which recovery steps ran.
#
# Note: the permutations' expected notifications (and the leaf pages that each
# concurrent session step splits or deletes) assume the default 8KB BLCKSZ.

setup
{
  CREATE EXTENSION injection_points;
  CREATE TABLE backwards_scan_tbl(col int4) WITH (autovacuum_enabled = off);
  CREATE INDEX ON backwards_scan_tbl(col) WITH (deduplicate_items = off);
  INSERT INTO backwards_scan_tbl SELECT i FROM generate_series(0, 700) i;
}
setup
{
  VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) backwards_scan_tbl;
}
teardown
{
  DROP EXTENSION injection_points;
  DROP TABLE backwards_scan_tbl;
}

session scan_session
setup {
  SELECT injection_points_set_local();
  SET enable_seqscan=off;
  SET enable_sort=off;
}
step b_attach {
  SELECT injection_points_attach('nbtree-walk-left', 'wait');
  SELECT injection_points_attach('nbtree-walk-left-step-right', 'notice');
  SELECT injection_points_attach('nbtree-walk-left-restart', 'notice');
  SELECT injection_points_attach('nbtree-walk-left-deleted', 'notice');
}
# Variant that doesn't attach to nbtree-walk-left-step-right, for
# permutations whose number of step right attempts varies with the amount of
# free space that index tuples' varying alignment padding leaves on each page
step b_attach_nosr {
  SELECT injection_points_attach('nbtree-walk-left', 'wait');
  SELECT injection_points_attach('nbtree-walk-left-restart', 'notice');
  SELECT injection_points_attach('nbtree-walk-left-deleted', 'notice');
}
# Note: Both scan variants call parallel restricted pg_backend_pid() so that
# the scan runs in the leader process under debug_parallel_query
step b_scan { SELECT col FROM backwards_scan_tbl
              WHERE col % 100 = 1 AND pg_backend_pid() <> 0
              ORDER BY col DESC; }
step b_scan_999 { SELECT col FROM backwards_scan_tbl
                  WHERE col <= 999 AND col % 100 = 1 AND pg_backend_pid() <> 0
                  ORDER BY col DESC; }
step b_detach {
  SELECT injection_points_detach('nbtree-walk-left-step-right');
  SELECT injection_points_detach('nbtree-walk-left-restart');
  SELECT injection_points_detach('nbtree-walk-left-deleted');
}
step b_detach_nosr {
  SELECT injection_points_detach('nbtree-walk-left-restart');
  SELECT injection_points_detach('nbtree-walk-left-deleted');
}

session concurrent_session
step i_insert { INSERT INTO backwards_scan_tbl SELECT i FROM generate_series(-2000, 700) i; }
step i_insert_dups { INSERT INTO backwards_scan_tbl SELECT 100 FROM generate_series(1, 60); }
step i_grow { INSERT INTO backwards_scan_tbl SELECT i FROM generate_series(701, 2200) i; }
step d_delete_left { DELETE FROM backwards_scan_tbl WHERE col < 601; }
step d_delete_mid { DELETE FROM backwards_scan_tbl WHERE col BETWEEN 367 AND 2100; }
step vacuum_tbl { VACUUM backwards_scan_tbl; }
step i_detach {
  SELECT injection_points_detach('nbtree-walk-left');
  SELECT injection_points_wakeup('nbtree-walk-left');
}

# A single concurrent page split.  When the backwards scan session wakes up,
# its search recovers by stepping right just once.
permutation b_attach
    b_scan
    i_insert_dups
    i_detach
    b_detach

# Many concurrent page splits.  When the backwards scan session wakes up, its
# search steps right the maximum number of times before giving up and
# starting over with the right sibling page's current left link.
permutation b_attach
    b_scan
    i_insert
    i_detach
    b_detach

# Concurrent deletion of all pages to the left of the page that the scan just
# read.  When the backwards scan session wakes up, its search determines that
# the scan has no page to the left to move to, ending the scan.
permutation b_attach
    d_delete_left
    b_scan
    vacuum_tbl
    i_detach
    b_detach

# Concurrent deletion of the page that the scan just read (which the scan can
# only safely rely on when a search locates its left sibling using its saved
# right link, which the deleted page's right sibling has acquired).  The scan
# just read a page whose tuples all pointed to dead-to-all heap tuples, which
# VACUUM deletes during the scan's wait, along with all nearby pages.
permutation b_attach_nosr
    i_grow
    d_delete_mid
    b_scan_999
    vacuum_tbl
    i_detach
    b_detach_nosr
