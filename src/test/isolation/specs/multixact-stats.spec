# Test for pg_get_multixact_stats()
#
# This test creates one multixact on a brand-new table.  While the multixact
# is pinned by two open transactions, we check some patterns that VACUUM and
# FREEZE cannot violate:
# 1) "members" increased by at least 1 when the second session locked the row.
# 2) (num_mxids / num_members) not decreased compared to earlier snapshots.
# 3) "oldest_*" fields never decreased.
#
# This test does not run checks after releasing locks, as freezing and/or
# truncation may shrink the multixact ranges calculated.

setup
{
    CREATE TABLE mxq(id int PRIMARY KEY, v int);
    INSERT INTO mxq VALUES (1, 42);
}

teardown
{
    DROP TABLE mxq;
}

# Two sessions that lock the same tuple, leading to one multixact with
# at least 2 members.
session "s1"
setup { SET client_min_messages = warning; SET lock_timeout = '5s'; }
step s1_begin  { BEGIN; }
step s1_lock   { SELECT 1 FROM mxq WHERE id=1 FOR KEY SHARE; }
step s1_commit { COMMIT; }

session "s2"
setup { SET client_min_messages = warning; SET lock_timeout = '5s'; }
step s2_begin  { BEGIN; }
step s2_lock   { SELECT 1 FROM mxq WHERE id=1 FOR KEY SHARE; }
step s2_commit { COMMIT; }

# Save multixact state *BEFORE* any locking; some of these may be NULLs if
# multixacts have not initialized yet.
step snap0 {
  CREATE TEMP TABLE snap0 AS
  SELECT num_mxids, num_members, oldest_multixact
  FROM pg_get_multixact_stats();
}

# Save multixact state after s1 has locked the row.
step snap1 {
  CREATE TEMP TABLE snap1 AS
  SELECT num_mxids, num_members, oldest_multixact
  FROM pg_get_multixact_stats();
}

# Save multixact state after s2 joins to lock the same row, leading to
# a multixact with at least 2 members.
step snap2 {
  CREATE TEMP TABLE snap2 AS
  SELECT num_mxids, num_members, oldest_multixact
  FROM pg_get_multixact_stats();
}

# Pretty, deterministic key/value outputs based of boolean checks:
#   is_init_mxids            : num_mxids not NULL
#   is_init_members          : num_members not NULL
#   is_init_oldest_mxid      : oldest_multixact not NULL
#   is_oldest_mxid_nondec_01 : oldest_multixact not decreased (snap0->snap1)
#   is_oldest_mxid_nondec_12 : oldest_multixact did not decreased (snap1->snap2)
#   is_members_increased_ge1 : members increased by at least 1 when s2 joined
#   is_mxids_nondec_01       : num_mxids not decreased (snap0->snap1)
#   is_mxids_nondec_12       : num_mxids not decreased (snap1->snap2)
#   is_members_nondec_01     : num_members not decreased (snap0->snap1)
#   is_members_nondec_12     : num_members not decreased (snap1->snap2)
step check_while_pinned {
  SELECT r.assertion, r.ok
  FROM snap0 s0
  JOIN snap1 s1 ON TRUE
  JOIN snap2 s2 ON TRUE,
  LATERAL unnest(
    ARRAY[
      'is_init_mxids',
      'is_init_members',
      'is_init_oldest_mxid',
      'is_init_oldest_off',
      'is_oldest_mxid_nondec_01',
      'is_oldest_mxid_nondec_12',
      'is_oldest_off_nondec_01',
      'is_oldest_off_nondec_12',
      'is_members_increased_ge1',
      'is_mxids_nondec_01',
      'is_mxids_nondec_12',
      'is_members_nondec_01',
      'is_members_nondec_12'
    ],
    ARRAY[
      (s2.num_mxids        IS NOT NULL),
      (s2.num_members      IS NOT NULL),
      (s2.oldest_multixact IS NOT NULL),

      (s1.oldest_multixact::text::bigint >= COALESCE(s0.oldest_multixact::text::bigint, 0)),
      (s2.oldest_multixact::text::bigint >= COALESCE(s1.oldest_multixact::text::bigint, 0)),

      (s2.num_members >= COALESCE(s1.num_members, 0) + 1),

      (s1.num_mxids   >= COALESCE(s0.num_mxids,   0)),
      (s2.num_mxids   >= COALESCE(s1.num_mxids,   0)),
      (s1.num_members >= COALESCE(s0.num_members, 0)),
      (s2.num_members >= COALESCE(s1.num_members, 0))
    ]
  ) AS r(assertion, ok);
}

permutation snap0 s1_begin s1_lock snap1 s2_begin s2_lock snap2 check_while_pinned s1_commit s2_commit
