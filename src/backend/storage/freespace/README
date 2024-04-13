src/backend/storage/freespace/README

Free Space Map
--------------

The purpose of the free space map is to quickly locate a page with enough
free space to hold a tuple to be stored; or to determine that no such page
exists and the relation must be extended by one page.  As of PostgreSQL 8.4
each relation has its own, extensible free space map stored in a separate
"fork" of its relation.  This eliminates the disadvantages of the former
fixed-size FSM.

It is important to keep the map small so that it can be searched rapidly.
Therefore, we don't attempt to record the exact free space on a page.
We allocate one map byte to each page, allowing us to record free space
at a granularity of 1/256th of a page.  Another way to say it is that
the stored value is the free space divided by BLCKSZ/256 (rounding down).
We assume that the free space must always be less than BLCKSZ, since
all pages have some overhead; so the maximum map value is 255.

To assist in fast searching, the map isn't simply an array of per-page
entries, but has a tree structure above those entries.  There is a tree
structure of pages, and a tree structure within each page, as described
below.

FSM page structure
------------------

Within each FSM page, we use a binary tree structure where leaf nodes store
the amount of free space on heap pages (or lower level FSM pages, see
"Higher-level structure" below), with one leaf node per heap page. A non-leaf
node stores the max amount of free space on any of its children.

For example:

    4
 4     2
3 4   0 2    <- This level represents heap pages

We need two basic operations: search and update.

To search for a page with X amount of free space, traverse down the tree
along a path where n >= X, until you hit the bottom. If both children of a
node satisfy the condition, you can pick either one arbitrarily.

To update the amount of free space on a page to X, first update the leaf node
corresponding to the heap page, then "bubble up" the change to upper nodes,
by walking up to each parent and recomputing its value as the max of its
two children.  Repeat until reaching the root or a parent whose value
doesn't change.

This data structure has a couple of nice properties:
- to discover that there is no page with X bytes of free space, you only
  need to look at the root node
- by varying which child to traverse to in the search algorithm, when you have
  a choice, we can implement various strategies, like preferring pages closer
  to a given page, or spreading the load across the table.

Higher-level routines that use FSM pages access them through the fsm_set_avail()
and fsm_search_avail() functions. The interface to those functions hides the
page's internal tree structure, treating the FSM page as a black box that has
a certain number of "slots" for storing free space information.  (However,
the higher routines have to be aware of the tree structure of the whole map.)

The binary tree is stored on each FSM page as an array. Because the page
header takes some space on a page, the binary tree isn't perfect. That is,
a few right-most leaf nodes are missing, and there are some useless non-leaf
nodes at the right. So the tree looks something like this:

       0
   1       2
 3   4   5   6
7 8 9 A B

where the numbers denote each node's position in the array.  Note that the
tree is guaranteed complete above the leaf level; only some leaf nodes are
missing.  This is reflected in the number of usable "slots" per page not
being an exact power of 2.

A FSM page also has a next slot pointer, fp_next_slot, that determines where
to start the next search for free space within that page.  The reason for that
is to spread out the pages that are returned by FSM searches.  When several
backends are concurrently inserting into a relation, contention can be avoided
by having them insert into different pages.  But it is also desirable to fill
up pages in sequential order, to get the benefit of OS prefetching and batched
writes.  The FSM is responsible for making that happen, and the next slot
pointer helps provide the desired behavior.

Higher-level structure
----------------------

To scale up the data structure described above beyond a single page, we
maintain a similar tree-structure across pages. Leaf nodes in higher level
pages correspond to lower level FSM pages. The root node within each page
has the same value as the corresponding leaf node on its parent page.

The root page is always stored at physical block 0.

For example, assuming each FSM page can hold information about 4 pages (in
reality, it holds (BLCKSZ - headers) / 2, or ~4000 with default BLCKSZ),
we get a disk layout like this:

 0     <-- page 0 at level 2 (root page)
  0     <-- page 0 at level 1
   0     <-- page 0 at level 0
   1     <-- page 1 at level 0
   2     <-- ...
   3
  1     <-- page 1 at level 1
   4
   5
   6
   7
  2
   8
   9
   10
   11
  3
   12
   13
   14
   15

where the numbers are page numbers *at that level*, starting from 0.

To find the physical block # corresponding to leaf page n, we need to
count the number of leaf and upper-level pages preceding page n.
This turns out to be

y = n + (n / F + 1) + (n / F^2 + 1) + ... + 1

where F is the fanout (4 in the above example). The first term n is the number
of preceding leaf pages, the second term is the number of pages at level 1,
and so forth.

To keep things simple, the tree is always constant height. To cover the
maximum relation size of 2^32-1 blocks, three levels is enough with the default
BLCKSZ (4000^3 > 2^32).

Addressing
----------

The higher-level routines operate on "logical" addresses, consisting of
- level,
- logical page number, and
- slot (if applicable)

Bottom level FSM pages have level of 0, the level above that 1, and root 2.
As in the diagram above, logical page number is the page number at that level,
starting from 0.

Locking
-------

When traversing down to search for free space, only one page is locked at a
time: the parent page is released before locking the child. If the child page
is concurrently modified, and there no longer is free space on the child page
when you land on it, you need to start from scratch (after correcting the
parent page, so that you don't get into an infinite loop).

We use shared buffer locks when searching, but exclusive buffer lock when
updating a page.  However, the next slot search pointer is updated during
searches even though we have only a shared lock.  fp_next_slot is just a hint
and we can easily reset it if it gets corrupted; so it seems better to accept
some risk of that type than to pay the overhead of exclusive locking.

Recovery
--------

The FSM is not explicitly WAL-logged. Instead, we rely on a bunch of
self-correcting measures to repair possible corruption.

First of all, whenever a value is set on an FSM page, the root node of the
page is compared against the new value after bubbling up the change is
finished. It should be greater than or equal to the value just set, or we
have a corrupted page, with a parent somewhere with too small a value.
Secondly, if we detect corrupted pages while we search, traversing down
the tree. That check will notice if a parent node is set to too high a value.
In both cases, the upper nodes on the page are immediately rebuilt, fixing
the corruption so far as that page is concerned.

VACUUM updates all the bottom-level FSM pages with the correct amount of free
space on corresponding heap pages, as it proceeds through the heap.  This
goes through fsm_set_avail(), so that the upper nodes on those pages are
immediately updated.  Periodically, VACUUM calls FreeSpaceMapVacuum[Range]
to propagate the new free-space info into the upper pages of the FSM tree.

As a result when we write to the FSM we treat that as a hint and thus use
MarkBufferDirtyHint() rather than MarkBufferDirty().  Every read here uses
RBM_ZERO_ON_ERROR to bypass checksum mismatches and other verification
failures.  We'd operate correctly without the full page images that
MarkBufferDirtyHint() provides, but they do decrease the chance of losing slot
knowledge to RBM_ZERO_ON_ERROR.

Relation extension is not WAL-logged.  Hence, after WAL replay, an on-disk FSM
slot may indicate free space in PageIsNew() blocks that never reached disk.
We detect this case by comparing against the actual relation size, and we mark
the block as full in that case.

TODO
----

- fastroot to avoid traversing upper nodes with just 1 child
- use a different system for tables that fit into one FSM page, with a
  mechanism to switch to the real thing as it grows.
