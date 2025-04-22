# Test race conditions involving:
# - s1: heap_update($FROM_SYSCACHE), without a snapshot or pin
# - s2: ALTER TABLE making $FROM_SYSCACHE a dead tuple
# - s3: "VACUUM pg_class" making $FROM_SYSCACHE become LP_UNUSED

# This is a derivative work of inplace.spec, which exercises the corresponding
# race condition for inplace updates.

# Despite local injection points, this is incompatible with runningcheck.
# First, removable_cutoff() could move backward, per its header comment.
# Second, other activity could trigger sinval queue overflow, negating our
# efforts to delay inval.  Third, this deadlock emerges:
#
# - step at2 waits at an injection point, with interrupts held
# - an unrelated backend waits for at2 to do PROCSIGNAL_BARRIER_SMGRRELEASE
# - step waitprunable4 waits for the unrelated backend to release its xmin

# The alternative expected output is for -DCATCACHE_FORCE_RELEASE, a setting
# that thwarts testing the race conditions this spec seeks.


# Need s2 to make a non-HOT update.  Otherwise, "VACUUM pg_class" would leave
# an LP_REDIRECT that persists.  To get non-HOT, make rels so the pg_class row
# for vactest.orig50 is on a filled page (assuming BLCKSZ=8192).  Just to save
# on filesystem syscalls, use relkind=c for every other rel.
setup
{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA vactest;
	-- Ensure a leader RELOID catcache entry.  PARALLEL RESTRICTED since a
	-- parallel worker running pg_relation_filenode() would lack that effect.
	CREATE FUNCTION vactest.reloid_catcache_set(regclass) RETURNS int
		LANGUAGE sql PARALLEL RESTRICTED
		AS 'SELECT 0 FROM pg_relation_filenode($1)';
	CREATE FUNCTION vactest.mkrels(text, int, int) RETURNS void
		LANGUAGE plpgsql SET search_path = vactest AS $$
	DECLARE
		tname text;
	BEGIN
		FOR i in $2 .. $3 LOOP
			tname := $1 || i;
			EXECUTE FORMAT('CREATE TYPE ' || tname || ' AS ()');
			RAISE DEBUG '% at %', tname, ctid
				FROM pg_class WHERE oid = tname::regclass;
		END LOOP;
	END
	$$;
	CREATE PROCEDURE vactest.wait_prunable() LANGUAGE plpgsql AS $$
	DECLARE
		barrier xid8;
		cutoff xid8;
	BEGIN
		barrier := pg_current_xact_id();
		-- autovacuum worker RelationCacheInitializePhase3() or the
		-- isolationtester control connection might hold a snapshot that
		-- limits pruning.  Sleep until that clears.  See comments at
		-- removable_cutoff() for why we pass a shared catalog rather than
		-- pg_class, the table we'll prune.
		LOOP
			ROLLBACK;  -- release MyProc->xmin, which could be the oldest
			cutoff := removable_cutoff('pg_database');
			EXIT WHEN cutoff >= barrier;
			RAISE LOG 'removable cutoff %; waiting for %', cutoff, barrier;
			PERFORM pg_sleep(.1);
		END LOOP;
	END
	$$;
}
# Eliminate HEAPTUPLE_DEAD and HEAPTUPLE_RECENTLY_DEAD from pg_class.
# Minimize free space.
#
# If we kept HEAPTUPLE_RECENTLY_DEAD, step vac4 could prune what we missed,
# breaking some permutation assumptions.  Specifically, the next pg_class
# tuple could end up in free space we failed to liberate here, instead of
# going in the specific free space vac4 intended to liberate for it.
setup	{ CALL vactest.wait_prunable();  -- maximize VACUUM FULL }
setup	{ VACUUM FULL pg_class;  -- reduce free space }
# Remove the one tuple that VACUUM FULL makes dead, a tuple pertaining to
# pg_class itself.  Populate the FSM for pg_class.
#
# wait_prunable waits for snapshots that would thwart pruning, while FREEZE
# waits for buffer pins that would thwart pruning.  DISABLE_PAGE_SKIPPING
# isn't actually needed, but other pruning-dependent tests use it.  If those
# tests remove it, remove it here.
setup	{ CALL vactest.wait_prunable();  -- maximize lazy VACUUM }
setup	{ VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pg_class;  -- fill fsm etc. }
setup
{
	SELECT FROM vactest.mkrels('orig', 1, 49);
	CREATE TABLE vactest.orig50 (c int) WITH (autovacuum_enabled = off);
	CREATE TABLE vactest.child50 (c int) WITH (autovacuum_enabled = off);
	SELECT FROM vactest.mkrels('orig', 51, 100);
}
teardown
{
	DROP SCHEMA vactest CASCADE;
	DROP EXTENSION injection_points;
}

# Wait during GRANT.  Disable debug_discard_caches, since we're here to
# exercise an outcome that happens under permissible cache staleness.
session s1
setup	{
	SET debug_discard_caches = 0;
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('heap_update-before-pin', 'wait');
}
step cachefill1	{ SELECT FROM vactest.reloid_catcache_set('vactest.orig50'); }
step grant1	{ GRANT SELECT ON vactest.orig50 TO PUBLIC; }

# Update of the tuple that grant1 will update.  Wait before sending invals, so
# s1 will not get a cache miss.  Choose the commands for making such updates
# from among those whose heavyweight locking does not conflict with GRANT's
# heavyweight locking.  (GRANT will see our XID as committed, so observing
# that XID in the tuple xmax also won't block GRANT.)
session s2
setup	{
	SELECT FROM injection_points_set_local();
	SELECT FROM
		injection_points_attach('transaction-end-process-inval', 'wait');
}
step at2	{
	CREATE TRIGGER to_set_relhastriggers BEFORE UPDATE ON vactest.orig50
		FOR EACH ROW EXECUTE PROCEDURE suppress_redundant_updates_trigger();
}

# Hold snapshot to block pruning.
session s3
step snap3	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT; }
step r3		{ ROLLBACK; }

# Non-blocking actions.
session s4
step waitprunable4	{ CALL vactest.wait_prunable(); }
# Eliminate HEAPTUPLE_DEAD.  See above discussion of FREEZE.
step vac4		{ VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pg_class; }
# Reuse the lp that s1 is waiting to change.  I've observed reuse at the 1st
# or 18th CREATE, so create excess.
step mkrels4	{
	SELECT FROM vactest.mkrels('intruder', 1, 100);  -- repopulate LP_UNUSED
}
step wakegrant4	{
	SELECT FROM injection_points_detach('heap_update-before-pin');
	SELECT FROM injection_points_wakeup('heap_update-before-pin');
}
step at4	{ ALTER TABLE vactest.child50 INHERIT vactest.orig50; }
step wakeinval4	{
	SELECT FROM injection_points_detach('transaction-end-process-inval');
	SELECT FROM injection_points_wakeup('transaction-end-process-inval');
}
# Witness effects of steps at2 and/or at4.
step inspect4	{
	SELECT relhastriggers, relhassubclass FROM pg_class
		WHERE oid = 'vactest.orig50'::regclass;
}

# TID from syscache becomes LP_UNUSED.  Before the bug fix, this permutation
# made s1 fail with "attempted to update invisible tuple" or an assert.
# However, suppose a pd_lsn value such that (pd_lsn.xlogid, pd_lsn.xrecoff)
# passed for (xmin, xmax) with xmin known-committed and xmax known-aborted.
# Persistent page header corruption ensued.  For example, s1 overwrote
# pd_lower, pd_upper, and pd_special as though they were t_ctid.
permutation
	cachefill1			# reads pg_class tuple T0, xmax invalid
	at2					# T0 dead, T1 live
	waitprunable4		# T0 prunable
	vac4				# T0 becomes LP_UNUSED
	grant1				# pauses at heap_update(T0)
	wakeinval4(at2)		# at2 sends inval message
	wakegrant4(grant1)	# s1 wakes: "tuple concurrently deleted"

# add mkrels4: LP_UNUSED becomes a different rel's row
permutation
	cachefill1			# reads pg_class tuple T0, xmax invalid
	at2					# T0 dead, T1 live
	waitprunable4		# T0 prunable
	vac4				# T0 becomes LP_UNUSED
	grant1				# pauses at heap_update(T0)
	wakeinval4(at2)		# at2 sends inval message
	mkrels4				# T0 becomes a new rel
	wakegrant4(grant1)	# s1 wakes: "duplicate key value violates unique"

# TID from syscache becomes LP_UNUSED, then becomes a newer version of the
# original rel's row.
permutation
	snap3				# sets MyProc->xmin
	cachefill1			# reads pg_class tuple T0, xmax invalid
	at2					# T0 dead, T1 live
	mkrels4				# T1's page becomes full
	r3					# clears MyProc->xmin
	waitprunable4		# T0 prunable
	vac4				# T0 becomes LP_UNUSED
	grant1				# pauses at heap_update(T0)
	wakeinval4(at2)		# at2 sends inval message
	at4					# T1 dead, T0 live
	wakegrant4(grant1)	# s1 wakes: T0 dead, T2 live
	inspect4			# observe loss of at2+at4 changes XXX is an extant bug
