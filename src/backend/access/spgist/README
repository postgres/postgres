src/backend/access/spgist/README

SP-GiST is an abbreviation of space-partitioned GiST.  It provides a
generalized infrastructure for implementing space-partitioned data
structures, such as quadtrees, k-d trees, and radix trees (tries).  When
implemented in main memory, these structures are usually designed as a set of
dynamically-allocated nodes linked by pointers.  This is not suitable for
direct storing on disk, since the chains of pointers can be rather long and
require too many disk accesses. In contrast, disk based data structures
should have a high fanout to minimize I/O.  The challenge is to map tree
nodes to disk pages in such a way that the search algorithm accesses only a
few disk pages, even if it traverses many nodes.


COMMON STRUCTURE DESCRIPTION

Logically, an SP-GiST tree is a set of tuples, each of which can be either
an inner or leaf tuple.  Each inner tuple contains "nodes", which are
(label,pointer) pairs, where the pointer (ItemPointerData) is a pointer to
another inner tuple or to the head of a list of leaf tuples.  Inner tuples
can have different numbers of nodes (children).  Branches can be of different
depth (actually, there is no control or code to support balancing), which
means that the tree is non-balanced.  However, leaf and inner tuples cannot
be intermixed at the same level: a downlink from a node of an inner tuple
leads either to one inner tuple, or to a list of leaf tuples.

The SP-GiST core requires that inner and leaf tuples fit on a single index
page, and even more stringently that the list of leaf tuples reached from a
single inner-tuple node all be stored on the same index page.  (Restricting
such lists to not cross pages reduces seeks, and allows the list links to be
stored as simple 2-byte OffsetNumbers.)  SP-GiST index opclasses should
therefore ensure that not too many nodes can be needed in one inner tuple,
and that inner-tuple prefixes and leaf-node datum values not be too large.

Inner and leaf tuples are stored separately: the former are stored only on
"inner" pages, the latter only on "leaf" pages.  Also, there are special
restrictions on the root page.  Early in an index's life, when there is only
one page's worth of data, the root page contains an unorganized set of leaf
tuples.  After the first page split has occurred, the root is required to
contain exactly one inner tuple.

When the search traversal algorithm reaches an inner tuple, it chooses a set
of nodes to continue tree traverse in depth.  If it reaches a leaf page it
scans a list of leaf tuples to find the ones that match the query. SP-GiST
also supports ordered (nearest-neighbor) searches - that is during scan pending
nodes are put into priority queue, so traversal is performed by the
closest-first model.


The insertion algorithm descends the tree similarly, except it must choose
just one node to descend to from each inner tuple.  Insertion might also have
to modify the inner tuple before it can descend: it could add a new node, or
it could "split" the tuple to obtain a less-specific prefix that can match
the value to be inserted.  If it's necessary to append a new leaf tuple to a
list and there is no free space on page, then SP-GiST creates a new inner
tuple and distributes leaf tuples into a set of lists on, perhaps, several
pages.

An inner tuple consists of:

  optional prefix value - all successors must be consistent with it.
    Example:
        radix tree   - prefix value is a common prefix string
        quad tree    - centroid
        k-d tree     - one coordinate

  list of nodes, where node is a (label, pointer) pair.
    Example of a label: a single character for radix tree

A leaf tuple consists of:

  a leaf value
    Example:
        radix tree - the rest of string (postfix)
        quad and k-d tree - the point itself

  ItemPointer to the corresponding heap tuple
  nextOffset number of next leaf tuple in a chain on a leaf page

  optional nulls bitmask
  optional INCLUDE-column values

For compatibility with pre-v14 indexes, a leaf tuple has a nulls bitmask
only if there are null values (among the leaf value and the INCLUDE values)
*and* there is at least one INCLUDE column.  The null-ness of the leaf
value can be inferred from whether the tuple is on a "nulls page" (see below)
so it is not necessary to represent it explicitly.  But we include it anyway
in a bitmask used with INCLUDE values, so that standard tuple deconstruction
code can be used.


NULLS HANDLING

We assume that SPGiST-indexable operators are strict (can never succeed for
null inputs).  It is still desirable to index nulls, so that whole-table
indexscans are possible and so that "x IS NULL" can be implemented by an
SPGiST indexscan.  However, we prefer that SPGiST index opclasses not have
to cope with nulls.  Therefore, the main tree of an SPGiST index does not
include any null entries.  We store null entries in a separate SPGiST tree
occupying a disjoint set of pages (in particular, its own root page).
Insertions and searches in the nulls tree do not use any of the
opclass-supplied functions, but just use hardwired logic comparable to
AllTheSame cases in the normal tree.


INSERTION ALGORITHM

Insertion algorithm is designed to keep the tree in a consistent state at
any moment.  Here is a simplified insertion algorithm specification
(numbers refer to notes below):

  Start with the first tuple on the root page (1)

  loop:
    if (page is leaf) then
        if (enough space)
            insert on page and exit (5)
        else (7)
            call PickSplitFn() (2)
        end if
    else
        switch (chooseFn())
            case MatchNode  - descend through selected node
            case AddNode    - add node and then retry chooseFn (3, 6)
            case SplitTuple - split inner tuple to prefix and postfix, then
                              retry chooseFn with the prefix tuple (4, 6)
    end if

Notes:

(1) Initially, we just dump leaf tuples into the root page until it is full;
then we split it.  Once the root is not a leaf page, it can have only one
inner tuple, so as to keep the amount of free space on the root as large as
possible.  Both of these rules are meant to postpone doing PickSplit on the
root for as long as possible, so that the topmost partitioning of the search
space is as good as we can easily make it.

(2) Current implementation allows to do picksplit and insert a new leaf tuple
in one operation, if the new list of leaf tuples fits on one page. It's
always possible for trees with small nodes like quad tree or k-d tree, but
radix trees may require another picksplit.

(3) Addition of node must keep size of inner tuple small enough to fit on a
page.  After addition, inner tuple could become too large to be stored on
current page because of other tuples on page. In this case it will be moved
to another inner page (see notes about page management). When moving tuple to
another page, we can't change the numbers of other tuples on the page, else
we'd make downlink pointers to them invalid. To prevent that, SP-GiST leaves
a "placeholder" tuple, which can be reused later whenever another tuple is
added to the page. See also Concurrency and Vacuum sections below. Right now
only radix trees could add a node to the tuple; quad trees and k-d trees
make all possible nodes at once in PickSplitFn() call.

(4) Prefix value could only partially match a new value, so the SplitTuple
action allows breaking the current tree branch into upper and lower sections.
Another way to say it is that we can split the current inner tuple into
"prefix" and "postfix" parts, where the prefix part is able to match the
incoming new value. Consider example of insertion into a radix tree. We use
the following notation, where tuple's id is just for discussion (no such id
is actually stored):

inner tuple: {tuple id}(prefix string)[ comma separated list of node labels ]
leaf tuple: {tuple id}<value>

Suppose we need to insert string 'www.gogo.com' into inner tuple

    {1}(www.google.com/)[a, i]

The string does not match the prefix so we cannot descend.  We must
split the inner tuple into two tuples:

    {2}(www.go)[o]  - prefix tuple
                |
                {3}(gle.com/)[a,i] - postfix tuple

On the next iteration of loop we find that 'www.gogo.com' matches the
prefix, but not any node label, so we add a node [g] to tuple {2}:

                   NIL (no child exists yet)
                   |
    {2}(www.go)[o, g]
                |
                {3}(gle.com/)[a,i]

Now we can descend through the [g] node, which will cause us to update
the target string to just 'o.com'.  Finally, we'll insert a leaf tuple
bearing that string:

                  {4}<o.com>
                   |
    {2}(www.go)[o, g]
                |
                {3}(gle.com/)[a,i]

As we can see, the original tuple's node array moves to postfix tuple without
any changes.  Note also that SP-GiST core assumes that prefix tuple is not
larger than old inner tuple.  That allows us to store prefix tuple directly
in place of old inner tuple.  SP-GiST core will try to store postfix tuple on
the same page if possible, but will use another page if there is not enough
free space (see notes 5 and 6).  Currently, quad and k-d trees don't use this
feature, because they have no concept of a prefix being "inconsistent" with
any new value.  They grow their depth only by PickSplitFn() call.

(5) If pointer from node of parent is a NIL pointer, algorithm chooses a leaf
page to store on.  At first, it tries to use the last-used leaf page with the
largest free space (which we track in each backend) to better utilize disk
space.  If that's not large enough, then the algorithm allocates a new page.

(6) Management of inner pages is very similar to management of leaf pages,
described in (5).

(7) Actually, current implementation can move the whole list of leaf tuples
and a new tuple to another page, if the list is short enough. This improves
space utilization, but doesn't change the basis of the algorithm.


CONCURRENCY

While descending the tree, the insertion algorithm holds exclusive lock on
two tree levels at a time, ie both parent and child pages (but parent and
child pages can be the same, see notes above).  There is a possibility of
deadlock between two insertions if there are cross-referenced pages in
different branches.  That is, if inner tuple on page M has a child on page N
while an inner tuple from another branch is on page N and has a child on
page M, then two insertions descending the two branches could deadlock,
since they will each hold their parent page's lock while trying to get the
child page's lock.

Currently, we deal with this by conditionally locking buffers as we descend
the tree.  If we fail to get lock on a buffer, we release both buffers and
restart the insertion process.  This is potentially inefficient, but the
locking costs of a more deterministic approach seem very high.

To reduce the number of cases where that happens, we introduce a concept of
"triple parity" of pages: if inner tuple is on page with BlockNumber N, then
its child tuples should be placed on the same page, or else on a page with
BlockNumber M where (N+1) mod 3 == M mod 3.  This rule ensures that tuples
on page M will have no children on page N, since (M+1) mod 3 != N mod 3.
That makes it unlikely that two insertion processes will conflict against
each other while descending the tree.  It's not perfect though: in the first
place, we could still get a deadlock among three or more insertion processes,
and in the second place, it's impractical to preserve this invariant in every
case when we expand or split an inner tuple.  So we still have to allow for
deadlocks.

Insertion may also need to take locks on an additional inner and/or leaf page
to add tuples of the right type(s), when there's not enough room on the pages
it descended through.  However, we don't care exactly which such page we add
to, so deadlocks can be avoided by conditionally locking the additional
buffers: if we fail to get lock on an additional page, just try another one.

Search traversal algorithm is rather traditional.  At each non-leaf level, it
share-locks the page, identifies which node(s) in the current inner tuple
need to be visited, and puts those addresses on a stack of pages to examine
later.  It then releases lock on the current buffer before visiting the next
stack item.  So only one page is locked at a time, and no deadlock is
possible.  But instead, we have to worry about race conditions: by the time
we arrive at a pointed-to page, a concurrent insertion could have replaced
the target inner tuple (or leaf tuple chain) with data placed elsewhere.
To handle that, whenever the insertion algorithm changes a nonempty downlink
in an inner tuple, it places a "redirect tuple" in place of the lower-level
inner tuple or leaf-tuple chain head that the link formerly led to.  Scans
(though not insertions) must be prepared to honor such redirects.  Only a
scan that had already visited the parent level could possibly reach such a
redirect tuple, so we can remove redirects once all active transactions have
been flushed out of the system.


DEAD TUPLES

Tuples on leaf pages can be in one of four states:

SPGIST_LIVE: normal, live pointer to a heap tuple.

SPGIST_REDIRECT: placeholder that contains a link to another place in the
index.  When a chain of leaf tuples has to be moved to another page, a
redirect tuple is inserted in place of the chain's head tuple.  The parent
inner tuple's downlink is updated when this happens, but concurrent scans
might be "in flight" from the parent page to the child page (since they
release lock on the parent page before attempting to lock the child).
The redirect pointer serves to tell such a scan where to go.  A redirect
pointer is only needed for as long as such concurrent scans could be in
progress.  Eventually, it's converted into a PLACEHOLDER dead tuple by
VACUUM, and is then a candidate for replacement.  Searches that find such
a tuple (which should never be part of a chain) should immediately proceed
to the other place, forgetting about the redirect tuple.  Insertions that
reach such a tuple should raise error, since a valid downlink should never
point to such a tuple.

SPGIST_DEAD: tuple is dead, but it cannot be removed or moved to a
different offset on the page because there is a link leading to it from
some inner tuple elsewhere in the index.  (Such a tuple is never part of a
chain, since we don't need one unless there is nothing live left in its
chain.)  Searches should ignore such entries.  If an insertion action
arrives at such a tuple, it should either replace it in-place (if there's
room on the page to hold the desired new leaf tuple) or replace it with a
redirection pointer to wherever it puts the new leaf tuple.

SPGIST_PLACEHOLDER: tuple is dead, and there are known to be no links to
it from elsewhere.  When a live tuple is deleted or moved away, and not
replaced by a redirect pointer, it is replaced by a placeholder to keep
the offsets of later tuples on the same page from changing.  Placeholders
can be freely replaced when adding a new tuple to the page, and also
VACUUM will delete any that are at the end of the range of valid tuple
offsets.  Both searches and insertions should complain if a link from
elsewhere leads them to a placeholder tuple.

When the root page is also a leaf, all its tuple should be in LIVE state;
there's no need for the others since there are no links and no need to
preserve offset numbers.

Tuples on inner pages can be in LIVE, REDIRECT, or PLACEHOLDER states.
The REDIRECT state has the same function as on leaf pages, to send
concurrent searches to the place where they need to go after an inner
tuple is moved to another page.  Expired REDIRECT pointers are converted
to PLACEHOLDER status by VACUUM, and are then candidates for replacement.
DEAD state is not currently possible, since VACUUM does not attempt to
remove unused inner tuples.


VACUUM

VACUUM (or more precisely, spgbulkdelete) performs a single sequential scan
over the entire index.  On both leaf and inner pages, we can convert old
REDIRECT tuples into PLACEHOLDER status, and then remove any PLACEHOLDERs
that are at the end of the page (since they aren't needed to preserve the
offsets of any live tuples).  On leaf pages, we scan for tuples that need
to be deleted because their heap TIDs match a vacuum target TID.

If we find a deletable tuple that is not at the head of its chain, we
can simply replace it with a PLACEHOLDER, updating the chain links to
remove it from the chain.  If it is at the head of its chain, but there's
at least one live tuple remaining in the chain, we move that live tuple
to the head tuple's offset, replacing it with a PLACEHOLDER to preserve
the offsets of other tuples.  This keeps the parent inner tuple's downlink
valid.  If we find ourselves deleting all live tuples in a chain, we
replace the head tuple with a DEAD tuple and the rest with PLACEHOLDERS.
The parent inner tuple's downlink thus points to the DEAD tuple, and the
rules explained in the previous section keep everything working.

VACUUM doesn't know a-priori which tuples are heads of their chains, but
it can easily figure that out by constructing a predecessor array that's
the reverse map of the nextOffset links (ie, when we see tuple x links to
tuple y, we set predecessor[y] = x).  Then head tuples are the ones with
no predecessor.

Because insertions can occur while VACUUM runs, a pure sequential scan
could miss deleting some target leaf tuples, because they could get moved
from a not-yet-visited leaf page to an already-visited leaf page as a
consequence of a PickSplit or MoveLeafs operation.  Failing to delete any
target TID is not acceptable, so we have to extend the algorithm to cope
with such cases.  We recognize that such a move might have occurred when
we see a leaf-page REDIRECT tuple whose XID indicates it might have been
created after the VACUUM scan started.  We add the redirection target TID
to a "pending list" of places we need to recheck.  Between pages of the
main sequential scan, we empty the pending list by visiting each listed
TID.  If it points to an inner tuple (from a PickSplit), add each downlink
TID to the pending list.  If it points to a leaf page, vacuum that page.
(We could just vacuum the single pointed-to chain, but vacuuming the
whole page simplifies the code and reduces the odds of VACUUM having to
modify the same page multiple times.)  To ensure that pending-list
processing can never get into an endless loop, even in the face of
concurrent index changes, we don't remove list entries immediately but
only after we've completed all pending-list processing; instead we just
mark items as done after processing them.  Adding a TID that's already in
the list is a no-op, whether or not that item is marked done yet.

spgbulkdelete also updates the index's free space map.

Currently, spgvacuumcleanup has nothing to do if spgbulkdelete was
performed; otherwise, it does an spgbulkdelete scan with an empty target
list, so as to clean up redirections and placeholders, update the free
space map, and gather statistics.


LAST USED PAGE MANAGEMENT

The list of last used pages contains four pages - a leaf page and three
inner pages, one from each "triple parity" group.  (Actually, there's one
such list for the main tree and a separate one for the nulls tree.)  This
list is stored between calls on the index meta page, but updates are never
WAL-logged to decrease WAL traffic.  Incorrect data on meta page isn't
critical, because we could allocate a new page at any moment.


AUTHORS

    Teodor Sigaev <teodor@sigaev.ru>
    Oleg Bartunov <oleg@sai.msu.su>
