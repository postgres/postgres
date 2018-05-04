src/backend/storage/lmgr/README-SSI

Serializable Snapshot Isolation (SSI) and Predicate Locking
===========================================================

This code is in the lmgr directory because about 90% of it is an
implementation of predicate locking, which is required for SSI,
rather than being directly related to SSI itself.  When another use
for predicate locking justifies the effort to tease these two things
apart, this README file should probably be split.


Credits
-------

This feature was developed by Kevin Grittner and Dan R. K. Ports,
with review and suggestions from Joe Conway, Heikki Linnakangas, and
Jeff Davis.  It is based on work published in these papers:

	Michael J. Cahill, Uwe Röhm, and Alan D. Fekete. 2008.
	Serializable isolation for snapshot databases.
	In SIGMOD '08: Proceedings of the 2008 ACM SIGMOD
	international conference on Management of data,
	pages 729-738, New York, NY, USA. ACM.
	http://doi.acm.org/10.1145/1376616.1376690

	Michael James Cahill. 2009.
	Serializable Isolation for Snapshot Databases.
	Sydney Digital Theses.
	University of Sydney, School of Information Technologies.
	http://hdl.handle.net/2123/5353


Overview
--------

With true serializable transactions, if you can show that your
transaction will do the right thing if there are no concurrent
transactions, it will do the right thing in any mix of serializable
transactions or be rolled back with a serialization failure.  This
feature has been implemented in PostgreSQL using SSI.


Serializable and Snapshot Transaction Isolation Levels
------------------------------------------------------

Serializable transaction isolation is attractive for shops with
active development by many programmers against a complex schema
because it guarantees data integrity with very little staff time --
if a transaction can be shown to always do the right thing when it is
run alone (before or after any other transaction), it will always do
the right thing in any mix of concurrent serializable transactions.
Where conflicts with other transactions would result in an
inconsistent state within the database or an inconsistent view of
the data, a serializable transaction will block or roll back to
prevent the anomaly. The SQL standard provides a specific SQLSTATE
for errors generated when a transaction rolls back for this reason,
so that transactions can be retried automatically.

Before version 9.1, PostgreSQL did not support a full serializable
isolation level. A request for serializable transaction isolation
actually provided snapshot isolation. This has well known anomalies
which can allow data corruption or inconsistent views of the data
during concurrent transactions; although these anomalies only occur
when certain patterns of read-write dependencies exist within a set
of concurrent transactions. Where these patterns exist, the anomalies
can be prevented by introducing conflicts through explicitly
programmed locks or otherwise unnecessary writes to the database.
Snapshot isolation is popular because performance is better than
serializable isolation and the integrity guarantees which it does
provide allow anomalies to be avoided or managed with reasonable
effort in many environments.


Serializable Isolation Implementation Strategies
------------------------------------------------

Techniques for implementing full serializable isolation have been
published and in use in many database products for decades. The
primary technique which has been used is Strict Two-Phase Locking
(S2PL), which operates by blocking writes against data which has been
read by concurrent transactions and blocking any access (read or
write) against data which has been written by concurrent
transactions. A cycle in a graph of blocking indicates a deadlock,
requiring a rollback. Blocking and deadlocks under S2PL in high
contention workloads can be debilitating, crippling throughput and
response time.

A new technique for implementing full serializable isolation in an
MVCC database appears in the literature beginning in 2008. This
technique, known as Serializable Snapshot Isolation (SSI) has many of
the advantages of snapshot isolation. In particular, reads don't
block anything and writes don't block reads. Essentially, it runs
snapshot isolation but monitors the read-write conflicts between
transactions to identify dangerous structures in the transaction
graph which indicate that a set of concurrent transactions might
produce an anomaly, and rolls back transactions to ensure that no
anomalies occur. It will produce some false positives (where a
transaction is rolled back even though there would not have been an
anomaly), but will never let an anomaly occur. In the two known
prototype implementations, performance for many workloads (even with
the need to restart transactions which are rolled back) is very close
to snapshot isolation and generally far better than an S2PL
implementation.


Apparent Serial Order of Execution
----------------------------------

One way to understand when snapshot anomalies can occur, and to
visualize the difference between the serializable implementations
described above, is to consider that among transactions executing at
the serializable transaction isolation level, the results are
required to be consistent with some serial (one-at-a-time) execution
of the transactions [1]. How is that order determined in each?

In S2PL, each transaction locks any data it accesses. It holds the
locks until committing, preventing other transactions from making
conflicting accesses to the same data in the interim. Some
transactions may have to be rolled back to prevent deadlock. But
successful transactions can always be viewed as having occurred
sequentially, in the order they committed.

With snapshot isolation, reads never block writes, nor vice versa, so
more concurrency is possible. The order in which transactions appear
to have executed is determined by something more subtle than in S2PL:
read/write dependencies. If a transaction reads data, it appears to
execute after the transaction that wrote the data it is reading.
Similarly, if it updates data, it appears to execute after the
transaction that wrote the previous version. These dependencies, which
we call "wr-dependencies" and "ww-dependencies", are consistent with
the commit order, because the first transaction must have committed
before the second starts. However, there can also be dependencies
between two *concurrent* transactions, i.e. where one was running when
the other acquired its snapshot.  These "rw-conflicts" occur when one
transaction attempts to read data which is not visible to it because
the transaction which wrote it (or will later write it) is
concurrent. The reading transaction appears to have executed first,
regardless of the actual sequence of transaction starts or commits,
because it sees a database state prior to that in which the other
transaction leaves it.

Anomalies occur when a cycle is created in the graph of dependencies:
when a dependency or series of dependencies causes transaction A to
appear to have executed before transaction B, but another series of
dependencies causes B to appear before A. If that's the case, then
the results can't be consistent with any serial execution of the
transactions.


SSI Algorithm
-------------

As of 9.1, serializable transactions in PostgreSQL are implemented using
Serializable Snapshot Isolation (SSI), based on the work of Cahill
et al. Fundamentally, this allows snapshot isolation to run as it
previously did, while monitoring for conditions which could create a
serialization anomaly.

SSI is based on the observation [2] that each snapshot isolation
anomaly corresponds to a cycle that contains a "dangerous structure"
of two adjacent rw-conflict edges:

      Tin ------> Tpivot ------> Tout
            rw             rw

SSI works by watching for this dangerous structure, and rolling
back a transaction when needed to prevent any anomaly. This means it
only needs to track rw-conflicts between concurrent transactions, not
wr- and ww-dependencies. It also means there is a risk of false
positives, because not every dangerous structure is embedded in an
actual cycle.  The number of false positives is low in practice, so
this represents an acceptable tradeoff for keeping the detection
overhead low.

The PostgreSQL implementation uses two additional optimizations:

* Tout must commit before any other transaction in the cycle
  (see proof of Theorem 2.1 of [2]). We only roll back a transaction
  if Tout commits before Tpivot and Tin.

* if Tin is read-only, there can only be an anomaly if Tout committed
  before Tin takes its snapshot. This optimization is an original
  one. Proof:

  - Because there is a cycle, there must be some transaction T0 that
    precedes Tin in the cycle. (T0 might be the same as Tout.)

  - The edge between T0 and Tin can't be a rw-conflict or ww-dependency,
    because Tin was read-only, so it must be a wr-dependency.
    Those can only occur if T0 committed before Tin took its snapshot,
    else Tin would have ignored T0's output.

  - Because Tout must commit before any other transaction in the
    cycle, it must commit before T0 commits -- and thus before Tin
    starts.


PostgreSQL Implementation
-------------------------

    * Since this technique is based on Snapshot Isolation (SI), those
areas in PostgreSQL which don't use SI can't be brought under SSI.
This includes system tables, temporary tables, sequences, hint bit
rewrites, etc.  SSI can not eliminate existing anomalies in these
areas.

    * Any transaction which is run at a transaction isolation level
other than SERIALIZABLE will not be affected by SSI.  If you want to
enforce business rules through SSI, all transactions should be run at
the SERIALIZABLE transaction isolation level, and that should
probably be set as the default.

    * If all transactions are run at the SERIALIZABLE transaction
isolation level, business rules can be enforced in triggers or
application code without ever having a need to acquire an explicit
lock or to use SELECT FOR SHARE or SELECT FOR UPDATE.

    * Those who want to continue to use snapshot isolation without
the additional protections of SSI (and the associated costs of
enforcing those protections), can use the REPEATABLE READ transaction
isolation level.  This level retains its legacy behavior, which
is identical to the old SERIALIZABLE implementation and fully
consistent with the standard's requirements for the REPEATABLE READ
transaction isolation level.

    * Performance under this SSI implementation will be significantly
improved if transactions which don't modify permanent tables are
declared to be READ ONLY before they begin reading data.

    * Performance under SSI will tend to degrade more rapidly with a
large number of active database transactions than under less strict
isolation levels.  Limiting the number of active transactions through
use of a connection pool or similar techniques may be necessary to
maintain good performance.

    * Any transaction which must be rolled back to prevent
serialization anomalies will fail with SQLSTATE 40001, which has a
standard meaning of "serialization failure".

    * This SSI implementation makes an effort to choose the
transaction to be canceled such that an immediate retry of the
transaction will not fail due to conflicts with exactly the same
transactions.  Pursuant to this goal, no transaction is canceled
until one of the other transactions in the set of conflicts which
could generate an anomaly has successfully committed.  This is
conceptually similar to how write conflicts are handled.  To fully
implement this guarantee there needs to be a way to roll back the
active transaction for another process with a serialization failure
SQLSTATE, even if it is "idle in transaction".


Predicate Locking
-----------------

Both S2PL and SSI require some form of predicate locking to handle
situations where reads conflict with later inserts or with later
updates which move data into the selected range.  PostgreSQL didn't
already have predicate locking, so it needed to be added to support
full serializable transactions under either strategy. Practical
implementations of predicate locking generally involve acquiring
locks against data as it is accessed, using multiple granularities
(tuple, page, table, etc.) with escalation as needed to keep the lock
count to a number which can be tracked within RAM structures.  This
approach was used in PostgreSQL.  Coarse granularities can cause some
false positive indications of conflict. The number of false positives
can be influenced by plan choice.


Implementation overview
-----------------------

New RAM structures, inspired by those used to track traditional locks
in PostgreSQL, but tailored to the needs of SIREAD predicate locking,
are used.  These refer to physical objects actually accessed in the
course of executing the query, to model the predicates through
inference.  Anyone interested in this subject should review the
Hellerstein, Stonebraker and Hamilton paper [3], along with the
locking papers referenced from that and the Cahill papers.

Because the SIREAD locks don't block, traditional locking techniques
have to be modified.  Intent locking (locking higher level objects
before locking lower level objects) doesn't work with non-blocking
"locks" (which are, in some respects, more like flags than locks).

A configurable amount of shared memory is reserved at postmaster
start-up to track predicate locks. This size cannot be changed
without a restart.

To prevent resource exhaustion, multiple fine-grained locks may
be promoted to a single coarser-grained lock as needed.

An attempt to acquire an SIREAD lock on a tuple when the same
transaction already holds an SIREAD lock on the page or the relation
will be ignored. Likewise, an attempt to lock a page when the
relation is locked will be ignored, and the acquisition of a coarser
lock will result in the automatic release of all finer-grained locks
it covers.


Heap locking
------------

Predicate locks will be acquired for the heap based on the following:

    * For a table scan, the entire relation will be locked.

    * Each tuple read which is visible to the reading transaction
will be locked, whether or not it meets selection criteria; except
that there is no need to acquire an SIREAD lock on a tuple when the
transaction already holds a write lock on any tuple representing the
row, since a rw-conflict would also create a ww-dependency which
has more aggressive enforcement and thus will prevent any anomaly.

    * Modifying a heap tuple creates a rw-conflict with any transaction
that holds a SIREAD lock on that tuple, or on the page or relation
that contains it.

    * Inserting a new tuple creates a rw-conflict with any transaction
holding a SIREAD lock on the entire relation. It doesn't conflict with
page-level locks, because page-level locks are only used to aggregate
tuple locks. Unlike index page locks, they don't lock "gaps" on the page.


Index AM implementations
------------------------

Since predicate locks only exist to detect writes which conflict with
earlier reads, and heap tuple locks are acquired to cover all heap
tuples actually read, including those read through indexes, the index
tuples which were actually scanned are not of interest in themselves;
we only care about their "new neighbors" -- later inserts into the
index which would have been included in the scan had they existed at
the time.  Conceptually, we want to lock the gaps between and
surrounding index entries within the scanned range.

Correctness requires that any insert into an index generates a
rw-conflict with a concurrent serializable transaction if, after that
insert, re-execution of any index scan of the other transaction would
access the heap for a row not accessed during the previous execution.
Note that a non-HOT update which expires an old index entry covered
by the scan and adds a new entry for the modified row's new tuple
need not generate a conflict, although an update which "moves" a row
into the scan must generate a conflict.  While correctness allows
false positives, they should be minimized for performance reasons.

Several optimizations are possible, though not all are implemented yet:

    * An index scan which is just finding the right position for an
index insertion or deletion need not acquire a predicate lock.

    * An index scan which is comparing for equality on the entire key
for a unique index need not acquire a predicate lock as long as a key
is found corresponding to a visible tuple which has not been modified
by another transaction -- there are no "between or around" gaps to
cover.

    * As long as built-in foreign key enforcement continues to use
its current "special tricks" to deal with MVCC issues, predicate
locks should not be needed for scans done by enforcement code.

    * If a search determines that no rows can be found regardless of
index contents because the search conditions are contradictory (e.g.,
x = 1 AND x = 2), then no predicate lock is needed.

Other index AM implementation considerations:

    * For an index AM that doesn't have support for predicate locking,
we just acquire a predicate lock on the whole index for any search.

    * B-tree index searches acquire predicate locks only on the
index *leaf* pages needed to lock the appropriate index range. If,
however, a search discovers that no root page has yet been created, a
predicate lock on the index relation is required.

    * Like a B-tree, GIN searches acquire predicate locks only on the
leaf pages of entry tree. When performing an equality scan, and an
entry has a posting tree, the posting tree root is locked instead, to
lock only that key value. However, fastupdate=on postpones the
insertion of tuples into index structure by temporarily storing them
into pending list. That makes us unable to detect r-w conflicts using
page-level locks. To cope with that, insertions to the pending list
conflict with all scans.

    * GiST searches can determine that there are no matches at any
level of the index, so we acquire predicate lock at each index
level during a GiST search. An index insert at the leaf level can
then be trusted to ripple up to all levels and locations where
conflicting predicate locks may exist. In case there is a page split,
we need to copy predicate lock from the original page to all the new
pages.

    * Hash index searches acquire predicate locks on the primary
page of a bucket. It acquires a lock on both the old and new buckets
for scans that happen concurrently with page splits. During a bucket
split, a predicate lock is copied from the primary page of an old
bucket to the primary page of a new bucket.

    * The effects of page splits, overflows, consolidations, and
removals must be carefully reviewed to ensure that predicate locks
aren't "lost" during those operations, or kept with pages which could
get re-used for different parts of the index.


Innovations
-----------

The PostgreSQL implementation of Serializable Snapshot Isolation
differs from what is described in the cited papers for several
reasons:

   1. PostgreSQL didn't have any existing predicate locking. It had
to be added from scratch.

   2. The existing in-memory lock structures were not suitable for
tracking SIREAD locks.
          * In PostgreSQL, tuple level locks are not held in RAM for
any length of time; lock information is written to the tuples
involved in the transactions.
          * In PostgreSQL, existing lock structures have pointers to
memory which is related to a session. SIREAD locks need to persist
past the end of the originating transaction and even the session
which ran it.
          * PostgreSQL needs to be able to tolerate a large number of
transactions executing while one long-running transaction stays open
-- the in-RAM techniques discussed in the papers wouldn't support
that.

   3. Unlike the database products used for the prototypes described
in the papers, PostgreSQL didn't already have a true serializable
isolation level distinct from snapshot isolation.

   4. PostgreSQL supports subtransactions -- an issue not mentioned
in the papers.

   5. PostgreSQL doesn't assign a transaction number to a database
transaction until and unless necessary (normally, when the transaction
attempts to modify data).

   6. PostgreSQL has pluggable data types with user-definable
operators, as well as pluggable index types, not all of which are
based around data types which support ordering.

   7. Some possible optimizations became apparent during development
and testing.

Differences from the implementation described in the papers are
listed below.

    * New structures needed to be created in shared memory to track
the proper information for serializable transactions and their SIREAD
locks.

    * Because PostgreSQL does not have the same concept of an "oldest
transaction ID" for all serializable transactions as assumed in the
Cahill thesis, we track the oldest snapshot xmin among serializable
transactions, and a count of how many active transactions use that
xmin. When the count hits zero we find the new oldest xmin and run a
clean-up based on that.

    * Because reads in a subtransaction may cause that subtransaction
to roll back, thereby affecting what is written by the top level
transaction, predicate locks must survive a subtransaction rollback.
As a consequence, all xid usage in SSI, including predicate locking,
is based on the top level xid.  When looking at an xid that comes
from a tuple's xmin or xmax, for example, we always call
SubTransGetTopmostTransaction() before doing much else with it.

    * PostgreSQL does not use "update in place" with a rollback log
for its MVCC implementation.  Where possible it uses "HOT" updates on
the same page (if there is room and no indexed value is changed).
For non-HOT updates the old tuple is expired in place and a new tuple
is inserted at a new location.  Because of this difference, a tuple
lock in PostgreSQL doesn't automatically lock any other versions of a
row.  We don't try to copy or expand a tuple lock to any other
versions of the row, based on the following proof that any additional
serialization failures we would get from that would be false
positives:

          o If transaction T1 reads a row version (thus acquiring a
predicate lock on it) and a second transaction T2 updates that row
version (thus creating a rw-conflict graph edge from T1 to T2), must a
third transaction T3 which re-updates the new version of the row also
have a rw-conflict in from T1 to prevent anomalies?  In other words,
does it matter whether we recognize the edge T1 -> T3?

          o If T1 has a conflict in, it certainly doesn't. Adding the
edge T1 -> T3 would create a dangerous structure, but we already had
one from the edge T1 -> T2, so we would have aborted something anyway.
(T2 has already committed, else T3 could not have updated its output;
but we would have aborted either T1 or T1's predecessor(s).  Hence
no cycle involving T1 and T3 can survive.)

          o Now let's consider the case where T1 doesn't have a
rw-conflict in. If that's the case, for this edge T1 -> T3 to make a
difference, T3 must have a rw-conflict out that induces a cycle in the
dependency graph, i.e. a conflict out to some transaction preceding T1
in the graph. (A conflict out to T1 itself would be problematic too,
but that would mean T1 has a conflict in, the case we already
eliminated.)

          o So now we're trying to figure out if there can be an
rw-conflict edge T3 -> T0, where T0 is some transaction that precedes
T1. For T0 to precede T1, there has to be some edge, or sequence of
edges, from T0 to T1. At least the last edge has to be a wr-dependency
or ww-dependency rather than a rw-conflict, because T1 doesn't have a
rw-conflict in. And that gives us enough information about the order
of transactions to see that T3 can't have a rw-conflict to T0:
 - T0 committed before T1 started (the wr/ww-dependency implies this)
 - T1 started before T2 committed (the T1->T2 rw-conflict implies this)
 - T2 committed before T3 started (otherwise, T3 would get aborted
                                   because of an update conflict)

          o That means T0 committed before T3 started, and therefore
there can't be a rw-conflict from T3 to T0.

          o So in all cases, we don't need the T1 -> T3 edge to
recognize cycles.  Therefore it's not necessary for T1's SIREAD lock
on the original tuple version to cover later versions as well.

    * Predicate locking in PostgreSQL starts at the tuple level
when possible. Multiple fine-grained locks are promoted to a single
coarser-granularity lock as needed to avoid resource exhaustion.  The
amount of memory used for these structures is configurable, to balance
RAM usage against SIREAD lock granularity.

    * Each backend keeps a process-local table of the locks it holds.
To support granularity promotion decisions with low CPU and locking
overhead, this table also includes the coarser covering locks and the
number of finer-granularity locks they cover.

    * Conflicts are identified by looking for predicate locks
when tuples are written, and by looking at the MVCC information when
tuples are read. There is no matching between two RAM-based locks.

    * Because write locks are stored in the heap tuples rather than a
RAM-based lock table, the optimization described in the Cahill thesis
which eliminates an SIREAD lock where there is a write lock is
implemented by the following:
         1. When checking a heap write for conflicts against existing
predicate locks, a tuple lock on the tuple being written is removed.
         2. When acquiring a predicate lock on a heap tuple, we
return quickly without doing anything if it is a tuple written by the
reading transaction.

    * Rather than using conflictIn and conflictOut pointers which use
NULL to indicate no conflict and a self-reference to indicate
multiple conflicts or conflicts with committed transactions, we use a
list of rw-conflicts. With the more complete information, false
positives are reduced and we have sufficient data for more aggressive
clean-up and other optimizations:

          o We can avoid ever rolling back a transaction until and
unless there is a pivot where a transaction on the conflict *out*
side of the pivot committed before either of the other transactions.

          o We can avoid ever rolling back a transaction when the
transaction on the conflict *in* side of the pivot is explicitly or
implicitly READ ONLY unless the transaction on the conflict *out*
side of the pivot committed before the READ ONLY transaction acquired
its snapshot. (An implicit READ ONLY transaction is one which
committed without writing, even though it was not explicitly declared
to be READ ONLY.)

          o We can more aggressively clean up conflicts, predicate
locks, and SSI transaction information.

    * We allow a READ ONLY transaction to "opt out" of SSI if there are
no READ WRITE transactions which could cause the READ ONLY
transaction to ever become part of a "dangerous structure" of
overlapping transaction dependencies.

    * We allow the user to request that a READ ONLY transaction wait
until the conditions are right for it to start in the "opt out" state
described above. We add a DEFERRABLE state to transactions, which is
specified and maintained in a way similar to READ ONLY. It is
ignored for transactions that are not SERIALIZABLE and READ ONLY.

    * When a transaction must be rolled back, we pick among the
active transactions such that an immediate retry will not fail again
on conflicts with the same transactions.

    * We use the PostgreSQL SLRU system to hold summarized
information about older committed transactions to put an upper bound
on RAM used. Beyond that limit, information spills to disk.
Performance can degrade in a pessimal situation, but it should be
tolerable, and transactions won't need to be canceled or blocked
from starting.


R&D Issues
----------

This is intended to be the place to record specific issues which need
more detailed review or analysis.

    * WAL file replay. While serializable implementations using S2PL
can guarantee that the write-ahead log contains commits in a sequence
consistent with some serial execution of serializable transactions,
SSI cannot make that guarantee. While the WAL replay is no less
consistent than under snapshot isolation, it is possible that under
PITR recovery or hot standby a database could reach a readable state
where some transactions appear before other transactions which would
have had to precede them to maintain serializable consistency. In
essence, if we do nothing, WAL replay will be at snapshot isolation
even for serializable transactions. Is this OK? If not, how do we
address it?

    * External replication. Look at how this impacts external
replication solutions, like Postgres-R, Slony, pgpool, HS/SR, etc.
This is related to the "WAL file replay" issue.

    * UNIQUE btree search for equality on all columns. Since a search
of a UNIQUE index using equality tests on all columns will lock the
heap tuple if an entry is found, it appears that there is no need to
get a predicate lock on the index in that case. A predicate lock is
still needed for such a search if a matching index entry which points
to a visible tuple is not found.

    * Minimize touching of shared memory. Should lists in shared
memory push entries which have just been returned to the front of the
available list, so they will be popped back off soon and some memory
might never be touched, or should we keep adding returned items to
the end of the available list?


References
----------

[1] http://www.contrib.andrew.cmu.edu/~shadow/sql/sql1992.txt
Search for serial execution to find the relevant section.

[2] A. Fekete et al. Making Snapshot Isolation Serializable. In ACM
Transactions on Database Systems 30:2, Jun. 2005.
http://dx.doi.org/10.1145/1071610.1071615

[3] Joseph M. Hellerstein, Michael Stonebraker and James Hamilton. 2007.
Architecture of a Database System. Foundations and Trends(R) in
Databases Vol. 1, No. 2 (2007) 141-259.
http://db.cs.berkeley.edu/papers/fntdb07-architecture.pdf
  Of particular interest:
    * 6.1 A Note on ACID
    * 6.2 A Brief Review of Serializability
    * 6.3 Locking and Latching
    * 6.3.1 Transaction Isolation Levels
    * 6.5.3 Next-Key Locking: Physical Surrogates for Logical Properties
