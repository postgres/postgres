Block Range Indexes (BRIN)
==========================

BRIN indexes intend to enable very fast scanning of extremely large tables.

The essential idea of a BRIN index is to keep track of summarizing values in
consecutive groups of heap pages (page ranges); for example, the minimum and
maximum values for datatypes with a btree opclass, or the bounding box for
geometric types.  These values can be used to avoid scanning such pages
during a table scan, depending on query quals.

The cost of this is having to update the stored summary values of each page
range as tuples are inserted into them.


Access Method Design
--------------------

Since item pointers are not stored inside indexes of this type, it is not
possible to support the amgettuple interface.  Instead, we only provide
amgetbitmap support.  The amgetbitmap routine returns a lossy TIDBitmap
comprising all pages in those page ranges that match the query
qualifications.  The recheck step in the BitmapHeapScan node prunes tuples
that are not visible according to the query qualifications.

An operator class must have the following entries:

- generic support procedures (pg_amproc), identical to all opclasses:
  * "opcinfo" (BRIN_PROCNUM_OPCINFO) initializes a structure for index
    creation or scanning
  * "addValue" (BRIN_PROCNUM_ADDVALUE) takes an index tuple and a heap item,
    and possibly changes the index tuple so that it includes the heap item
    values
  * "consistent" (BRIN_PROCNUM_CONSISTENT) takes an index tuple and query
    quals, and returns whether the index tuple values match the query quals.
  * "union" (BRIN_PROCNUM_UNION) takes two index tuples and modifies the first
    one so that it represents the union of the two.
Procedure numbers up to 10 are reserved for future expansion.

Additionally, each opclass needs additional support functions:
- Minmax-style operator classes:
  * Proc numbers 11-14 are used for the functions implementing inequality
    operators for the type, in this order: less than, less or equal,
    greater or equal, greater than.

Opclasses using a different design will require different additional procedure
numbers.

Operator classes also need to have operator (pg_amop) entries so that the
optimizer can choose the index to execute queries.
- Minmax-style operator classes:
  * The same operators as btree (<=, <, =, >=, >)

Each index tuple stores some NULL bits and some opclass-specified values, which
are stored in a single null bitmask of length twice the number of columns.  The
generic NULL bits indicate, for each column:
  * bt_hasnulls: Whether there's any NULL value at all in the page range
  * bt_allnulls: Whether all values are NULLs in the page range

The opclass-specified values are:
- Minmax-style operator classes
  * minimum value across all tuples in the range
  * maximum value across all tuples in the range

Note that the addValue and Union support procedures  must be careful to
datumCopy() the values they want to store in the in-memory BRIN tuple, and
must pfree() the old copies when replacing older ones.  Since some values
referenced from the tuple persist and others go away, there is no
well-defined lifetime for a memory context that would make this automatic.


The Range Map
-------------

To find the index tuple for a particular page range, we have an internal
structure we call the range map, or "revmap" for short.  This stores one TID
per page range, which is the address of the index tuple summarizing that
range.  Since the map entries are fixed size, it is possible to compute the
address of the range map entry for any given heap page by simple arithmetic.

When a new heap tuple is inserted in a summarized page range, we compare the
existing index tuple with the new heap tuple.  If the heap tuple is outside
the summarization data given by the index tuple for any indexed column (or
if the new heap tuple contains null values but the index tuple indicates
there are no nulls), the index is updated with the new values.  In many
cases it is possible to update the index tuple in-place, but if the new
index tuple is larger than the old one and there's not enough space in the
page, it is necessary to create a new index tuple with the new values.  The
range map can be updated quickly to point to it; the old index tuple is
removed.

If the range map points to an invalid TID, the corresponding page range is
considered to be not summarized.  When tuples are added to unsummarized
pages, nothing needs to happen.

To scan a table following a BRIN index, we scan the range map sequentially.
This yields index tuples in ascending page range order.  Query quals are
matched to each index tuple; if they match, each page within the page range
is returned as part of the output TID bitmap.  If there's no match, they are
skipped.  Range map entries returning invalid index TIDs, that is
unsummarized page ranges, are also returned in the TID bitmap.

The revmap is stored in the first few blocks of the index main fork,
immediately following the metapage.  Whenever the revmap needs to be
extended by another page, existing tuples in that page are moved to some
other page.

Heap tuples can be removed from anywhere without restriction.  It might be
useful to mark the corresponding index tuple somehow, if the heap tuple is
one of the constraining values of the summary data (i.e. either min or max
in the case of a btree-opclass-bearing datatype), so that in the future we
are aware of the need to re-execute summarization on that range, leading to
a possible tightening of the summary values.

Summarization
-------------

At index creation time, the whole table is scanned; for each page range the
summarizing values of each indexed column and nulls bitmap are collected and
stored in the index.  The partially-filled page range at the end of the
table is also summarized.

As new tuples get inserted at the end of the table, they may update the
index tuple that summarizes the partial page range at the end.  Eventually
that page range is complete and new tuples belong in a new page range that
hasn't yet been summarized.  Those insertions do not create a new index
entry; instead, the page range remains unsummarized until later.

Whenever VACUUM is run on the table, all unsummarized page ranges are
summarized.  This action can also be invoked by the user via
brin_summarize_new_values().  Both these procedures scan all the
unsummarized ranges, and create a summary tuple.  Again, this includes the
partially-filled page range at the end of the table.

Vacuuming
---------

Since no heap TIDs are stored in a BRIN index, it's not necessary to scan the
index when heap tuples are removed.  It might be that some summary values can
be tightened if heap tuples have been deleted; but this would represent an
optimization opportunity only, not a correctness issue.  It's simpler to
represent this as the need to re-run summarization on the affected page range
rather than "subtracting" values from the existing one.  This is not
currently implemented.

Note that if there are no indexes on the table other than the BRIN index,
usage of maintenance_work_mem by vacuum can be decreased significantly, because
no detailed index scan needs to take place (and thus it's not necessary for
vacuum to save TIDs to remove).  It's unlikely that BRIN would be the only
indexes in a table, though, because primary keys can be btrees only, and so
we don't implement this optimization.


Optimizer
---------

The optimizer selects the index based on the operator class' pg_amop
entries for the column.


Future improvements
-------------------

* Different-size page ranges?
  In the current design, each "index entry" in a BRIN index covers the same
  number of pages.  There's no hard reason for this; it might make sense to
  allow the index to self-tune so that some index entries cover smaller page
  ranges, if this allows the summary values to be more compact.  This would incur
  larger BRIN overhead for the index itself, but might allow better pruning of
  page ranges during scan.  In the limit of one index tuple per page, the index
  itself would occupy too much space, even though we would be able to skip
  reading the most heap pages, because the summary values are tight; in the
  opposite limit of a single tuple that summarizes the whole table, we wouldn't
  be able to prune anything even though the index is very small.  This can
  probably be made to work by using the range map as an index in itself.

* More compact representation for TIDBitmap?
  TIDBitmap is the structure used to represent bitmap scans.  The
  representation of lossy page ranges is not optimal for our purposes, because
  it uses a Bitmapset to represent pages in the range; since we're going to return
  all pages in a large range, it might be more convenient to allow for a
  struct that uses start and end page numbers to represent the range, instead.

* Better vacuuming?
  It might be useful to enable passing more useful info to BRIN indexes during
  vacuuming about tuples that are deleted, i.e. do not require the callback to
  pass each tuple's TID.  For instance we might need a callback that passes a
  block number instead of a TID.  That would help determine when to re-run
  summarization on blocks that have seen lots of tuple deletions.
