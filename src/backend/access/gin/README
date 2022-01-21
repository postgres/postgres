src/backend/access/gin/README

Gin for PostgreSQL
==================

Gin was sponsored by jfg://networks (http://www.jfg-networks.com/)

Gin stands for Generalized Inverted Index and should be considered as a genie,
not a drink.

Generalized means that the index does not know which operation it accelerates.
It instead works with custom strategies, defined for specific data types (read
"Index Method Strategies" in the PostgreSQL documentation). In that sense, Gin
is similar to GiST and differs from btree indices, which have predefined,
comparison-based operations.

An inverted index is an index structure storing a set of (key, posting list)
pairs, where 'posting list' is a set of heap rows in which the key occurs.
(A text document would usually contain many keys.)  The primary goal of
Gin indices is support for highly scalable, full-text search in PostgreSQL.

A Gin index consists of a B-tree index constructed over key values,
where each key is an element of some indexed items (element of array, lexeme
for tsvector) and where each tuple in a leaf page contains either a pointer to
a B-tree over item pointers (posting tree), or a simple list of item pointers
(posting list) if the list is small enough.

Note: There is no delete operation in the key (entry) tree. The reason for
this is that in our experience, the set of distinct words in a large corpus
changes very slowly.  This greatly simplifies the code and concurrency
algorithms.

Core PostgreSQL includes built-in Gin support for one-dimensional arrays
(eg. integer[], text[]).  The following operations are available:

  * contains: value_array @> query_array
  * overlaps: value_array && query_array
  * is contained by: value_array <@ query_array

Synopsis
--------

=# create index txt_idx on aa using gin(a);

Features
--------

  * Concurrency
  * Write-Ahead Logging (WAL).  (Recoverability from crashes.)
  * User-defined opclasses.  (The scheme is similar to GiST.)
  * Optimized index creation (Makes use of maintenance_work_mem to accumulate
    postings in memory.)
  * Text search support via an opclass
  * Soft upper limit on the returned results set using a GUC variable:
    gin_fuzzy_search_limit

Gin Fuzzy Limit
---------------

There are often situations when a full-text search returns a very large set of
results.  Since reading tuples from the disk and sorting them could take a
lot of time, this is unacceptable for production.  (Note that the search
itself is very fast.)

Such queries usually contain very frequent lexemes, so the results are not
very helpful. To facilitate execution of such queries Gin has a configurable
soft upper limit on the size of the returned set, determined by the
'gin_fuzzy_search_limit' GUC variable.  This is set to 0 by default (no
limit).

If a non-zero search limit is set, then the returned set is a subset of the
whole result set, chosen at random.

"Soft" means that the actual number of returned results could differ
from the specified limit, depending on the query and the quality of the
system's random number generator.

From experience, a value of 'gin_fuzzy_search_limit' in the thousands
(eg. 5000-20000) works well.  This means that 'gin_fuzzy_search_limit' will
have no effect for queries returning a result set with less tuples than this
number.

Index structure
---------------

The "items" that a GIN index indexes are composite values that contain
zero or more "keys".  For example, an item might be an integer array, and
then the keys would be the individual integer values.  The index actually
stores and searches for the key values, not the items per se.  In the
pg_opclass entry for a GIN opclass, the opcintype is the data type of the
items, and the opckeytype is the data type of the keys.  GIN is optimized
for cases where items contain many keys and the same key values appear
in many different items.

A GIN index contains a metapage, a btree of key entries, and possibly
"posting tree" pages, which hold the overflow when a key entry acquires
too many heap tuple pointers to fit in a btree page.  Additionally, if the
fast-update feature is enabled, there can be "list pages" holding "pending"
key entries that haven't yet been merged into the main btree.  The list
pages have to be scanned linearly when doing a search, so the pending
entries should be merged into the main btree before there get to be too
many of them.  The advantage of the pending list is that bulk insertion of
a few thousand entries can be much faster than retail insertion.  (The win
comes mainly from not having to do multiple searches/insertions when the
same key appears in multiple new heap tuples.)

Key entries are nominally of the same IndexTuple format as used in other
index types, but since a leaf key entry typically refers to multiple heap
tuples, there are significant differences.  (See GinFormTuple, which works
by building a "normal" index tuple and then modifying it.)  The points to
know are:

* In a single-column index, a key tuple just contains the key datum, but
in a multi-column index, a key tuple contains the pair (column number,
key datum) where the column number is stored as an int2.  This is needed
to support different key data types in different columns.  This much of
the tuple is built by index_form_tuple according to the usual rules.
The column number (if present) can never be null, but the key datum can
be, in which case a null bitmap is present as usual.  (As usual for index
tuples, the size of the null bitmap is fixed at INDEX_MAX_KEYS.)

* If the key datum is null (ie, IndexTupleHasNulls() is true), then
just after the nominal index data (ie, at offset IndexInfoFindDataOffset
or IndexInfoFindDataOffset + sizeof(int2)) there is a byte indicating
the "category" of the null entry.  These are the possible categories:
	1 = ordinary null key value extracted from an indexable item
	2 = placeholder for zero-key indexable item
	3 = placeholder for null indexable item
Placeholder null entries are inserted into the index because otherwise
there would be no index entry at all for an empty or null indexable item,
which would mean that full index scans couldn't be done and various corner
cases would give wrong answers.  The different categories of null entries
are treated as distinct keys by the btree, but heap itempointers for the
same category of null entry are merged into one index entry just as happens
with ordinary key entries.

* In a key entry at the btree leaf level, at the next SHORTALIGN boundary,
there is a list of item pointers, in compressed format (see Posting List
Compression section), pointing to the heap tuples for which the indexable
items contain this key. This is called the "posting list".

If the list would be too big for the index tuple to fit on an index page, the
ItemPointers are pushed out to a separate posting page or pages, and none
appear in the key entry itself.  The separate pages are called a "posting
tree" (see below); Note that in either case, the ItemPointers associated with
a key can easily be read out in sorted order; this is relied on by the scan
algorithms.

* The index tuple header fields of a leaf key entry are abused as follows:

1) Posting list case:

* ItemPointerGetBlockNumber(&itup->t_tid) contains the offset from index
  tuple start to the posting list.
  Access macros: GinGetPostingOffset(itup) / GinSetPostingOffset(itup,n)

* ItemPointerGetOffsetNumber(&itup->t_tid) contains the number of elements
  in the posting list (number of heap itempointers).
  Access macros: GinGetNPosting(itup) / GinSetNPosting(itup,n)

* If IndexTupleHasNulls(itup) is true, the null category byte can be
  accessed/set with GinGetNullCategory(itup,gs) / GinSetNullCategory(itup,gs,c)

* The posting list can be accessed with GinGetPosting(itup)

* If GinItupIsCompressed(itup), the posting list is stored in compressed
  format. Otherwise it is just an array of ItemPointers. New tuples are always
  stored in compressed format, uncompressed items can be present if the
  database was migrated from 9.3 or earlier version.

2) Posting tree case:

* ItemPointerGetBlockNumber(&itup->t_tid) contains the index block number
  of the root of the posting tree.
  Access macros: GinGetPostingTree(itup) / GinSetPostingTree(itup, blkno)

* ItemPointerGetOffsetNumber(&itup->t_tid) contains the magic number
  GIN_TREE_POSTING, which distinguishes this from the posting-list case
  (it's large enough that that many heap itempointers couldn't possibly
  fit on an index page).  This value is inserted automatically by the
  GinSetPostingTree macro.

* If IndexTupleHasNulls(itup) is true, the null category byte can be
  accessed/set with GinGetNullCategory(itup,gs) / GinSetNullCategory(itup,gs,c)

* The posting list is not present and must not be accessed.

Use the macro GinIsPostingTree(itup) to determine which case applies.

In both cases, itup->t_info & INDEX_SIZE_MASK contains actual total size of
tuple, and the INDEX_VAR_MASK and INDEX_NULL_MASK bits have their normal
meanings as set by index_form_tuple.

Index tuples in non-leaf levels of the btree contain the optional column
number, key datum, and null category byte as above.  They do not contain
a posting list.  ItemPointerGetBlockNumber(&itup->t_tid) is the downlink
to the next lower btree level, and ItemPointerGetOffsetNumber(&itup->t_tid)
is InvalidOffsetNumber.  Use the access macros GinGetDownlink/GinSetDownlink
to get/set the downlink.

Index entries that appear in "pending list" pages work a tad differently as
well.  The optional column number, key datum, and null category byte are as
for other GIN index entries.  However, there is always exactly one heap
itempointer associated with a pending entry, and it is stored in the t_tid
header field just as in non-GIN indexes.  There is no posting list.
Furthermore, the code that searches the pending list assumes that all
entries for a given heap tuple appear consecutively in the pending list and
are sorted by the column-number-plus-key-datum.  The GIN_LIST_FULLROW page
flag bit tells whether entries for a given heap tuple are spread across
multiple pending-list pages.  If GIN_LIST_FULLROW is set, the page contains
all the entries for one or more heap tuples.  If GIN_LIST_FULLROW is clear,
the page contains entries for only one heap tuple, *and* they are not all
the entries for that tuple.  (Thus, a heap tuple whose entries do not all
fit on one pending-list page must have those pages to itself, even if this
results in wasting much of the space on the preceding page and the last
page for the tuple.)

GIN packs downlinks and pivot keys into internal page tuples in a different way
than nbtree does.  Lehman & Yao defines it as following.

P_0, K_1, P_1, K_2, P_2, ... , K_n, P_n, K_{n+1}

There P_i is a downlink and K_i is a key.  K_i splits key space between P_{i-1}
and P_i (0 <= i <= n).  K_{n+1} is high key.

In internal page tuple is key and downlink grouped together.  nbtree packs
keys and downlinks into tuples as following.

(K_{n+1}, None), (-Inf, P_0), (K_1, P_1), ... , (K_n, P_n)

There tuples are shown in parentheses.  So, highkey is stored separately.  P_i
is grouped with K_i.  P_0 is grouped with -Inf key.

GIN packs keys and downlinks into tuples in a different way.

(P_0, K_1), (P_1, K_2), ... , (P_n, K_{n+1})

P_i is grouped with K_{i+1}.  -Inf key is not needed.

There are couple of additional notes regarding K_{n+1} key.
1) In entry tree rightmost page, a key coupled with P_n doesn't really matter.
Highkey is assumed to be infinity.
2) In posting tree, a key coupled with P_n always doesn't matter.  Highkey for
non-rightmost pages is stored separately and accessed via
GinDataPageGetRightBound().

Posting tree
------------

If a posting list is too large to store in-line in a key entry, a posting tree
is created. A posting tree is a B-tree structure, where the ItemPointer is
used as the key.

Internal posting tree pages use the standard PageHeader and the same "opaque"
struct as other GIN page, but do not contain regular index tuples. Instead,
the contents of the page is an array of PostingItem structs. Each PostingItem
consists of the block number of the child page, and the right bound of that
child page, as an ItemPointer. The right bound of the page is stored right
after the page header, before the PostingItem array.

Posting tree leaf pages also use the standard PageHeader and opaque struct,
and the right bound of the page is stored right after the page header, but
the page content comprises of a number of compressed posting lists. The
compressed posting lists are stored one after each other, between page header
and pd_lower. The space between pd_lower and pd_upper is unused, which allows
full-page images of posting tree leaf pages to skip the unused space in middle
(buffer_std = true in XLogRecData).

The item pointers are stored in a number of independent compressed posting
lists (also called segments), instead of one big one, to make random access
to a given item pointer faster: to find an item in a compressed list, you
have to read the list from the beginning, but when the items are split into
multiple lists, you can first skip over to the list containing the item you're
looking for, and read only that segment. Also, an update only needs to
re-encode the affected segment.

Posting List Compression
------------------------

To fit as many item pointers on a page as possible, posting tree leaf pages
and posting lists stored inline in entry tree leaf tuples use a lightweight
form of compression. We take advantage of the fact that the item pointers
are stored in sorted order. Instead of storing the block and offset number of
each item pointer separately, we store the difference from the previous item.
That in itself doesn't do much, but it allows us to use so-called varbyte
encoding to compress them.

Varbyte encoding is a method to encode integers, allowing smaller numbers to
take less space at the cost of larger numbers. Each integer is represented by
variable number of bytes. High bit of each byte in varbyte encoding determines
whether the next byte is still part of this number. Therefore, to read a single
varbyte encoded number, you have to read bytes until you find a byte with the
high bit not set.

When encoding, the block and offset number forming the item pointer are
combined into a single integer. The offset number is stored in the 11 low
bits (see MaxHeapTuplesPerPageBits in ginpostinglist.c), and the block number
is stored in the higher bits. That requires 43 bits in total, which
conveniently fits in at most 6 bytes.

A compressed posting list is passed around and stored on disk in a
GinPostingList struct. The first item in the list is stored uncompressed
as a regular ItemPointerData, followed by the length of the list in bytes,
followed by the packed items.

Concurrency
-----------

The entry tree and each posting tree are B-trees, with right-links connecting
sibling pages at the same level.  This is the same structure that is used in
the regular B-tree indexam (invented by Lehman & Yao), but we don't support
scanning a GIN trees backwards, so we don't need left-links.  The entry tree
leaves don't have dedicated high keys, instead greatest leaf tuple serves as
high key.  That works because tuples are never deleted from the entry tree.

The algorithms used to operate entry and posting trees are considered below.

### Locating the leaf page

When we search for leaf page in GIN btree to perform a read, we descend from
the root page to the leaf through using downlinks taking pin and shared lock on
one page at once.  So, we release pin and shared lock on previous page before
getting them on the next page.

The picture below shows tree state after finding the leaf page.  Lower case
letters depicts tree pages.  'S' depicts shared lock on the page.

               a
           /   |   \
       b       c       d
     / | \     | \     | \
   eS  f   g   h   i   j   k

### Steping right

Concurrent page splits move the keyspace to right, so after following a
downlink, the page actually containing the key we're looking for might be
somewhere to the right of the page we landed on.  In that case, we follow the
right-links until we find the page we're looking for.

During stepping right we take pin and shared lock on the right sibling before
releasing them from the current page.  This mechanism was designed to protect
from stepping to delete page.  We step to the right sibling while hold lock on
the rightlink pointing there.  So, it's guaranteed that nobody updates rightlink
concurrently and doesn't delete right sibling accordingly.

The picture below shows two pages locked at once during stepping right.

               a
           /   |   \
       b       c       d
     / | \     | \     | \
   eS  fS  g   h   i   j   k

### Insert

While finding appropriate leaf for insertion we also descend from the root to
leaf, while shared locking one page at once in.  But during insertion we don't
release pins from root and internal pages.  That could save us some lookups to
the buffers hash table for downlinks insertion assuming parents are not changed
due to concurrent splits.  Once we reach leaf we re-lock the page in exclusive
mode.

The picture below shows leaf page locked in exclusive mode and ready for
insertion.  'P' and 'E' depict pin and exclusive lock correspondingly.


               aP
           /   |   \
       b       cP      d
     / | \     | \     | \
   e   f   g   hE  i   j   k


If insert causes a page split, the parent is locked in exclusive mode before
unlocking the left child.  So, insertion algorithm can exclusively lock both
parent and child pages at once starting from child.

The picture below shows tree state after leaf page split.  'q' is new page
produced by split.  Parent 'c' is about to have downlink inserted.

                  aP
            /     |   \
       b          cE      d
     / | \      / | \     | \
   e   f   g  hE  q   i   j   k


### Page deletion

Vacuum never deletes tuples or pages from the entry tree. It traverses entry
tree leafs in logical order by rightlinks and removes deletable TIDs from
posting lists. Posting trees are processed by links from entry tree leafs. They
are vacuumed in two stages. At first stage, deletable TIDs are removed from
leafs. If first stage detects at least one empty page, then at the second stage
ginScanToDelete() deletes empty pages.

ginScanToDelete() traverses the whole tree in depth-first manner.  It starts
from the full cleanup lock on the tree root.  This lock prevents all the
concurrent insertions into this tree while we're deleting pages.  However,
there are still might be some in-progress readers, who traversed root before
we locked it.

The picture below shows tree state after page deletion algorithm traversed to
leftmost leaf of the tree.

               aE
           /   |   \
       bE      c       d
     / | \     | \     | \
   eE  f   g   h   i   j   k

Deletion algorithm keeps exclusive locks on left siblings of pages comprising
currently investigated path.  Thus, if current page is to be removed, all
required pages to remove both downlink and rightlink are already locked.  That
avoids potential right to left page locking order, which could deadlock with
concurrent stepping right.

A search concurrent to page deletion might already have read a pointer to the
page to be deleted, and might be just about to follow it.  A page can be reached
via the right-link of its left sibling, or via its downlink in the parent.

To prevent a backend from reaching a deleted page via a right-link, stepping
right algorithm doesn't release lock on the current page until lock of the
right page is acquired.

The downlink is more tricky.  A search descending the tree must release the lock
on the parent page before locking the child, or it could deadlock with a
concurrent split of the child page; a page split locks the parent, while already
holding a lock on the child page.  So, deleted page cannot be reclaimed
immediately.  Instead, we have to wait for every transaction, which might wait
to reference this page, to finish.  Corresponding processes must observe that
the page is marked deleted and recover accordingly.

The picture below shows tree state after page deletion algorithm further
traversed the tree.  Currently investigated path is 'a-c-h'.  Left siblings 'b'
and 'g' of 'c' and 'h' correspondingly are also exclusively locked.

               aE
           /   |   \
       bE      cE      d
     / | \     | \     | \
   e   f   gE  hE  i   j   k

The next picture shows tree state after page 'h' was deleted.  It's marked with
'deleted' flag and newest xid, which might visit it.  Downlink from 'c' to 'h'
is also deleted.

               aE
           /   |   \
       bE      cE      d
     / | \       \     | \
   e   f   gE  hD  iE  j   k

However, it's still possible that concurrent reader has seen downlink from 'c'
to 'h' before we deleted it.  In that case this reader will step right from 'h'
to till find non-deleted page.  Xid-marking of page 'h' guarantees that this
page wouldn't be reused till all such readers gone.  Next leaf page under
investigation is 'i'.  'g' remains locked as it becomes left sibling of 'i'.

The next picture shows tree state after 'i' and 'c' was deleted.  Internal page
'c' was deleted because it appeared to have no downlinks.  The path under
investigation is 'a-d-j'.  Pages 'b' and 'g' are locked as self siblings of 'd'
and 'j'.

               aE
           /       \
       bE      cD      dE
     / | \             | \
   e   f   gE  hD  iD  jE  k

During the replay of page deletion at standby, the page's left sibling, the
target page, and its parent, are locked in that order.  This order guarantees
no deadlock with concurrent reads.

Predicate Locking
-----------------

GIN supports predicate locking, for serializable snapshot isolation.
A predicate locks represent that a scan has scanned a range of values.  They
are not concerned with physical pages as such, but the logical key values.
A predicate lock on a page covers the key range that would belong on that
page, whether or not there are any matching tuples there currently.  In other
words, a predicate lock on an index page covers the "gaps" between the index
tuples.  To minimize false positives, predicate locks are acquired at the
finest level possible.

* Like in the B-tree index, it is enough to lock only leaf pages, because all
  insertions happen at the leaf level.

* In an equality search (i.e. not a partial match search), if a key entry has
  a posting tree, we lock the posting tree root page, to represent a lock on
  just that key entry.  Otherwise, we lock the entry tree page.  We also lock
  the entry tree page if no match is found, to lock the "gap" where the entry
  would've been, had there been one.

* In a partial match search, we lock all the entry leaf pages that we scan,
  in addition to locks on posting tree roots, to represent the "gaps" between
  values.

* In addition to the locks on entry leaf pages and posting tree roots, all
  scans grab a lock the metapage.  This is to interlock with insertions to
  the fast update pending list.  An insertion to the pending list can really
  belong anywhere in the tree, and the lock on the metapage represents that.

The interlock for fastupdate pending lists means that with fastupdate=on,
we effectively always grab a full-index lock, so you could get a lot of false
positives.

Compatibility
-------------

Compression of TIDs was introduced in 9.4. Some GIN indexes could remain in
uncompressed format because of pg_upgrade from 9.3 or earlier versions.
For compatibility, old uncompressed format is also supported. Following
rules are used to handle it:

* GIN_ITUP_COMPRESSED flag marks index tuples that contain a posting list.
This flag is stored in high bit of ItemPointerGetBlockNumber(&itup->t_tid).
Use GinItupIsCompressed(itup) to check the flag.

* Posting tree pages in the new format are marked with the GIN_COMPRESSED flag.
  Macros GinPageIsCompressed(page) and GinPageSetCompressed(page) are used to
  check and set this flag.

* All scan operations check format of posting list add use corresponding code
to read its content.

* When updating an index tuple containing an uncompressed posting list, it
will be replaced with new index tuple containing a compressed list.

* When updating an uncompressed posting tree leaf page, it's compressed.

* If vacuum finds some dead TIDs in uncompressed posting lists, they are
converted into compressed posting lists. This assumes that the compressed
posting list fits in the space occupied by the uncompressed list. IOW, we
assume that the compressed version of the page, with the dead items removed,
takes less space than the old uncompressed version.

Limitations
-----------

  * Gin doesn't use scan->kill_prior_tuple & scan->ignore_killed_tuples
  * Gin searches entries only by equality matching, or simple range
    matching using the "partial match" feature.

TODO
----

Nearest future:

  * Opclasses for more types (no programming, just many catalog changes)

Distant future:

  * Replace B-tree of entries to something like GiST

Authors
-------

Original work was done by Teodor Sigaev (teodor@sigaev.ru) and Oleg Bartunov
(oleg@sai.msu.su).
