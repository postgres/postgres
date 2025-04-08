--
-- Test assorted system views
--
-- This test is mainly meant to provide some code coverage for the
-- set-returning functions that underlie certain system views.
-- The output of most of these functions is very environment-dependent,
-- so our ability to test with fixed expected output is pretty limited;
-- but even a trivial check of count(*) will exercise the normal code path
-- through the SRF.

select count(*) >= 0 as ok from pg_available_extension_versions;

select count(*) >= 0 as ok from pg_available_extensions;

-- The entire output of pg_backend_memory_contexts is not stable,
-- we test only the existence and basic condition of TopMemoryContext.
select type, name, ident, level, total_bytes >= free_bytes
  from pg_backend_memory_contexts where level = 1;

-- We can exercise some MemoryContext type stats functions.  Most of the
-- column values are too platform-dependant to display.

-- Ensure stats from the bump allocator look sane.  Bump isn't a commonly
-- used context, but it is used in tuplesort.c, so open a cursor to keep
-- the tuplesort alive long enough for us to query the context stats.
begin;
declare cur cursor for select left(a,10), b
  from (values(repeat('a', 512 * 1024),1),(repeat('b', 512),2)) v(a,b)
  order by v.a desc;
fetch 1 from cur;
select type, name, total_bytes > 0, total_nblocks, free_bytes > 0, free_chunks
from pg_backend_memory_contexts where name = 'Caller tuples';
rollback;

-- Further sanity checks on pg_backend_memory_contexts.  We expect
-- CacheMemoryContext to have multiple children.  Ensure that's the case.
with contexts as (
  select * from pg_backend_memory_contexts
)
select count(*) > 1
from contexts c1, contexts c2
where c2.name = 'CacheMemoryContext'
and c1.path[c2.level] = c2.path[c2.level];

-- At introduction, pg_config had 23 entries; it may grow
select count(*) > 20 as ok from pg_config;

-- We expect no cursors in this test; see also portals.sql
select count(*) = 0 as ok from pg_cursors;

select count(*) >= 0 as ok from pg_file_settings;

-- There will surely be at least one rule, with no errors.
select count(*) > 0 as ok, count(*) FILTER (WHERE error IS NOT NULL) = 0 AS no_err
  from pg_hba_file_rules;

-- There may be no rules, and there should be no errors.
select count(*) >= 0 as ok, count(*) FILTER (WHERE error IS NOT NULL) = 0 AS no_err
  from pg_ident_file_mappings;

-- There will surely be at least one active lock
select count(*) > 0 as ok from pg_locks;

-- We expect no prepared statements in this test; see also prepare.sql
select count(*) = 0 as ok from pg_prepared_statements;

-- See also prepared_xacts.sql
select count(*) >= 0 as ok from pg_prepared_xacts;

-- There will surely be at least one SLRU cache
select count(*) > 0 as ok from pg_stat_slru;

-- There must be only one record
select count(*) = 1 as ok from pg_stat_wal;

-- We expect no walreceiver running in this test
select count(*) = 0 as ok from pg_stat_wal_receiver;

-- This is to record the prevailing planner enable_foo settings during
-- a regression test run.
select name, setting from pg_settings where name like 'enable%';

-- There are always wait event descriptions for various types.  InjectionPoint
-- may be present or absent, depending on history since last postmaster start.
select type, count(*) > 0 as ok FROM pg_wait_events
  where type <> 'InjectionPoint' group by type order by type COLLATE "C";

-- Test that the pg_timezone_names and pg_timezone_abbrevs views are
-- more-or-less working.  We can't test their contents in any great detail
-- without the outputs changing anytime IANA updates the underlying data,
-- but it seems reasonable to expect at least one entry per major meridian.
-- (At the time of writing, the actual counts are around 38 because of
-- zones using fractional GMT offsets, so this is a pretty loose test.)
select count(distinct utc_offset) >= 24 as ok from pg_timezone_names;
select count(distinct utc_offset) >= 24 as ok from pg_timezone_abbrevs;
-- Let's check the non-default timezone abbreviation sets, too
set timezone_abbreviations = 'Australia';
select count(distinct utc_offset) >= 24 as ok from pg_timezone_abbrevs;
set timezone_abbreviations = 'India';
select count(distinct utc_offset) >= 24 as ok from pg_timezone_abbrevs;
-- One specific case we can check without much fear of breakage
-- is the historical local-mean-time value used for America/Los_Angeles.
select * from pg_timezone_abbrevs where abbrev = 'LMT';

DO $$
DECLARE
    bg_writer_pid int;
    r RECORD;
BEGIN
        SELECT pid from pg_stat_activity where backend_type='background writer'
	 INTO bg_writer_pid;

        select type, name, ident
        from pg_get_process_memory_contexts(bg_writer_pid, false, 20)
	 where path = '{1}' into r;
	RAISE NOTICE '%', r;
        select type, name, ident
        from pg_get_process_memory_contexts(pg_backend_pid(), false, 20)
	 where path = '{1}' into r;
	RAISE NOTICE '%', r;
END $$;
