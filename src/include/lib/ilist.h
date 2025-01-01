/*-------------------------------------------------------------------------
 *
 * ilist.h
 *		integrated/inline doubly- and singly-linked lists
 *
 * These list types are useful when there are only a predetermined set of
 * lists that an object could be in.  List links are embedded directly into
 * the objects, and thus no extra memory management overhead is required.
 * (Of course, if only a small proportion of existing objects are in a list,
 * the link fields in the remainder would be wasted space.  But usually,
 * it saves space to not have separately-allocated list nodes.)
 *
 * The doubly-linked list comes in 2 forms.  dlist_head defines a head of a
 * doubly-linked list of dlist_nodes, whereas dclist_head defines the head of
 * a doubly-linked list of dlist_nodes with an additional 'count' field to
 * keep track of how many items are contained within the given list.  For
 * simplicity, dlist_head and dclist_head share the same node and iterator
 * types.  The functions to manipulate a dlist_head always have a name
 * starting with "dlist", whereas functions to manipulate a dclist_head have a
 * name starting with "dclist".  dclist_head comes with an additional function
 * (dclist_count) to return the number of entries in the list.  dclists are
 * able to store a maximum of PG_UINT32_MAX elements.  It is up to the caller
 * to ensure no more than this many items are added to a dclist.
 *
 * None of the functions here allocate any memory; they just manipulate
 * externally managed memory.  With the exception doubly-linked count lists
 * providing the ability to obtain the number of items in the list, the APIs
 * for singly and both doubly linked lists are identical as far as
 * capabilities of both allow.
 *
 * Each list has a list header, which exists even when the list is empty.
 * An empty singly-linked list has a NULL pointer in its header.
 *
 * For both doubly-linked list types, there are two valid ways to represent an
 * empty list.  The head's 'next' pointer can either be NULL or the head's
 * 'next' and 'prev' links can both point back to the list head (circular).
 * (If a dlist is modified and then all its elements are deleted, it will be
 * in the circular state.).  We prefer circular dlists because there are some
 * operations that can be done without branches (and thus faster) on lists
 * that use circular representation.  However, it is often convenient to
 * initialize list headers to zeroes rather than setting them up with an
 * explicit initialization function, so we also allow the NULL initialization.
 *
 * EXAMPLES
 *
 * Here's a simple example demonstrating how this can be used.  Let's assume
 * we want to store information about the tables contained in a database.
 *
 * #include "lib/ilist.h"
 *
 * // Define struct for the databases including a list header that will be
 * // used to access the nodes in the table list later on.
 * typedef struct my_database
 * {
 *		char	   *datname;
 *		dlist_head	tables;
 *		// ...
 * } my_database;
 *
 * // Define struct for the tables.  Note the list_node element which stores
 * // prev/next list links.  The list_node element need not be first.
 * typedef struct my_table
 * {
 *		char	   *tablename;
 *		dlist_node	list_node;
 *		perm_t		permissions;
 *		// ...
 * } my_table;
 *
 * // create a database
 * my_database *db = create_database();
 *
 * // and add a few tables to its table list
 * dlist_push_head(&db->tables, &create_table(db, "a")->list_node);
 * ...
 * dlist_push_head(&db->tables, &create_table(db, "b")->list_node);
 *
 *
 * To iterate over the table list, we allocate an iterator variable and use
 * a specialized looping construct.  Inside a dlist_foreach, the iterator's
 * 'cur' field can be used to access the current element.  iter.cur points to
 * a 'dlist_node', but most of the time what we want is the actual table
 * information; dlist_container() gives us that, like so:
 *
 * dlist_iter	iter;
 * dlist_foreach(iter, &db->tables)
 * {
 *		my_table   *tbl = dlist_container(my_table, list_node, iter.cur);
 *		printf("we have a table: %s in database %s\n",
 *			   tbl->tablename, db->datname);
 * }
 *
 *
 * While a simple iteration is useful, we sometimes also want to manipulate
 * the list while iterating.  There is a different iterator element and looping
 * construct for that.  Suppose we want to delete tables that meet a certain
 * criterion:
 *
 * dlist_mutable_iter miter;
 * dlist_foreach_modify(miter, &db->tables)
 * {
 *		my_table   *tbl = dlist_container(my_table, list_node, miter.cur);
 *
 *		if (!tbl->to_be_deleted)
 *			continue;		// don't touch this one
 *
 *		// unlink the current table from the linked list
 *		dlist_delete(miter.cur);
 *		// as these lists never manage memory, we can still access the table
 *		// after it's been unlinked
 *		drop_table(db, tbl);
 * }
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/lib/ilist.h
 *-------------------------------------------------------------------------
 */
#ifndef ILIST_H
#define ILIST_H

/*
 * Enable for extra debugging. This is rather expensive, so it's not enabled by
 * default even when USE_ASSERT_CHECKING.
 */
/* #define ILIST_DEBUG */

/*
 * Node of a doubly linked list.
 *
 * Embed this in structs that need to be part of a doubly linked list.
 */
typedef struct dlist_node dlist_node;
struct dlist_node
{
	dlist_node *prev;
	dlist_node *next;
};

/*
 * Head of a doubly linked list.
 *
 * Non-empty lists are internally circularly linked.  Circular lists have the
 * advantage of not needing any branches in the most common list manipulations.
 * An empty list can also be represented as a pair of NULL pointers, making
 * initialization easier.
 */
typedef struct dlist_head
{
	/*
	 * head.next either points to the first element of the list; to &head if
	 * it's a circular empty list; or to NULL if empty and not circular.
	 *
	 * head.prev either points to the last element of the list; to &head if
	 * it's a circular empty list; or to NULL if empty and not circular.
	 */
	dlist_node	head;
} dlist_head;


/*
 * Doubly linked list iterator type for dlist_head and dclist_head types.
 *
 * Used as state in dlist_foreach() and dlist_reverse_foreach() (and the
 * dclist variant thereof).
 *
 * To get the current element of the iteration use the 'cur' member.
 *
 * Iterations using this are *not* allowed to change the list while iterating!
 *
 * NB: We use an extra "end" field here to avoid multiple evaluations of
 * arguments in the dlist_foreach() and dclist_foreach() macros.
 */
typedef struct dlist_iter
{
	dlist_node *cur;			/* current element */
	dlist_node *end;			/* last node we'll iterate to */
} dlist_iter;

/*
 * Doubly linked list iterator for both dlist_head and dclist_head types.
 * This iterator type allows some modifications while iterating.
 *
 * Used as state in dlist_foreach_modify() and dclist_foreach_modify().
 *
 * To get the current element of the iteration use the 'cur' member.
 *
 * Iterations using this are only allowed to change the list at the current
 * point of iteration. It is fine to delete the current node, but it is *not*
 * fine to insert or delete adjacent nodes.
 *
 * NB: We need a separate type for mutable iterations so that we can store
 * the 'next' node of the current node in case it gets deleted or modified.
 */
typedef struct dlist_mutable_iter
{
	dlist_node *cur;			/* current element */
	dlist_node *next;			/* next node we'll iterate to */
	dlist_node *end;			/* last node we'll iterate to */
} dlist_mutable_iter;

/*
 * Head of a doubly linked list with a count of the number of items
 *
 * This internally makes use of a dlist to implement the actual list.  When
 * items are added or removed from the list the count is updated to reflect
 * the current number of items in the list.
 */
typedef struct dclist_head
{
	dlist_head	dlist;			/* the actual list header */
	uint32		count;			/* the number of items in the list */
} dclist_head;

/*
 * Node of a singly linked list.
 *
 * Embed this in structs that need to be part of a singly linked list.
 */
typedef struct slist_node slist_node;
struct slist_node
{
	slist_node *next;
};

/*
 * Head of a singly linked list.
 *
 * Singly linked lists are not circularly linked, in contrast to doubly linked
 * lists; we just set head.next to NULL if empty.  This doesn't incur any
 * additional branches in the usual manipulations.
 */
typedef struct slist_head
{
	slist_node	head;
} slist_head;

/*
 * Singly linked list iterator.
 *
 * Used as state in slist_foreach(). To get the current element of the
 * iteration use the 'cur' member.
 *
 * It's allowed to modify the list while iterating, with the exception of
 * deleting the iterator's current node; deletion of that node requires
 * care if the iteration is to be continued afterward.  (Doing so and also
 * deleting or inserting adjacent list elements might misbehave; also, if
 * the user frees the current node's storage, continuing the iteration is
 * not safe.)
 *
 * NB: this wouldn't really need to be an extra struct, we could use an
 * slist_node * directly. We prefer a separate type for consistency.
 */
typedef struct slist_iter
{
	slist_node *cur;
} slist_iter;

/*
 * Singly linked list iterator allowing some modifications while iterating.
 *
 * Used as state in slist_foreach_modify(). To get the current element of the
 * iteration use the 'cur' member.
 *
 * The only list modification allowed while iterating is to remove the current
 * node via slist_delete_current() (*not* slist_delete()).  Insertion or
 * deletion of nodes adjacent to the current node would misbehave.
 */
typedef struct slist_mutable_iter
{
	slist_node *cur;			/* current element */
	slist_node *next;			/* next node we'll iterate to */
	slist_node *prev;			/* prev node, for deletions */
} slist_mutable_iter;


/* Static initializers */
#define DLIST_STATIC_INIT(name) {{&(name).head, &(name).head}}
#define DCLIST_STATIC_INIT(name) {{{&(name).dlist.head, &(name).dlist.head}}, 0}
#define SLIST_STATIC_INIT(name) {{NULL}}


/* Prototypes for functions too big to be inline */

/* Caution: this is O(n); consider using slist_delete_current() instead */
extern void slist_delete(slist_head *head, const slist_node *node);

#ifdef ILIST_DEBUG
extern void dlist_member_check(const dlist_head *head, const dlist_node *node);
extern void dlist_check(const dlist_head *head);
extern void slist_check(const slist_head *head);
#else
/*
 * These seemingly useless casts to void are here to keep the compiler quiet
 * about the argument being unused in many functions in a non-debug compile,
 * in which functions the only point of passing the list head pointer is to be
 * able to run these checks.
 */
#define dlist_member_check(head, node) ((void) (head))
#define dlist_check(head)	((void) (head))
#define slist_check(head)	((void) (head))
#endif							/* ILIST_DEBUG */

/* doubly linked list implementation */

/*
 * Initialize a doubly linked list.
 * Previous state will be thrown away without any cleanup.
 */
static inline void
dlist_init(dlist_head *head)
{
	head->head.next = head->head.prev = &head->head;
}

/*
 * Initialize a doubly linked list element.
 *
 * This is only needed when dlist_node_is_detached() may be needed.
 */
static inline void
dlist_node_init(dlist_node *node)
{
	node->next = node->prev = NULL;
}

/*
 * Is the list empty?
 *
 * An empty list has either its first 'next' pointer set to NULL, or to itself.
 */
static inline bool
dlist_is_empty(const dlist_head *head)
{
	dlist_check(head);

	return head->head.next == NULL || head->head.next == &(head->head);
}

/*
 * Insert a node at the beginning of the list.
 */
static inline void
dlist_push_head(dlist_head *head, dlist_node *node)
{
	if (head->head.next == NULL)	/* convert NULL header to circular */
		dlist_init(head);

	node->next = head->head.next;
	node->prev = &head->head;
	node->next->prev = node;
	head->head.next = node;

	dlist_check(head);
}

/*
 * Insert a node at the end of the list.
 */
static inline void
dlist_push_tail(dlist_head *head, dlist_node *node)
{
	if (head->head.next == NULL)	/* convert NULL header to circular */
		dlist_init(head);

	node->next = &head->head;
	node->prev = head->head.prev;
	node->prev->next = node;
	head->head.prev = node;

	dlist_check(head);
}

/*
 * Insert a node after another *in the same list*
 */
static inline void
dlist_insert_after(dlist_node *after, dlist_node *node)
{
	node->prev = after;
	node->next = after->next;
	after->next = node;
	node->next->prev = node;
}

/*
 * Insert a node before another *in the same list*
 */
static inline void
dlist_insert_before(dlist_node *before, dlist_node *node)
{
	node->prev = before->prev;
	node->next = before;
	before->prev = node;
	node->prev->next = node;
}

/*
 * Delete 'node' from its list (it must be in one).
 */
static inline void
dlist_delete(dlist_node *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

/*
 * Like dlist_delete(), but also sets next/prev to NULL to signal not being in
 * a list.
 */
static inline void
dlist_delete_thoroughly(dlist_node *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->next = NULL;
	node->prev = NULL;
}

/*
 * Same as dlist_delete, but performs checks in ILIST_DEBUG builds to ensure
 * that 'node' belongs to 'head'.
 */
static inline void
dlist_delete_from(dlist_head *head, dlist_node *node)
{
	dlist_member_check(head, node);
	dlist_delete(node);
}

/*
 * Like dlist_delete_from, but also sets next/prev to NULL to signal not
 * being in a list.
 */
static inline void
dlist_delete_from_thoroughly(dlist_head *head, dlist_node *node)
{
	dlist_member_check(head, node);
	dlist_delete_thoroughly(node);
}

/*
 * Remove and return the first node from a list (there must be one).
 */
static inline dlist_node *
dlist_pop_head_node(dlist_head *head)
{
	dlist_node *node;

	Assert(!dlist_is_empty(head));
	node = head->head.next;
	dlist_delete(node);
	return node;
}

/*
 * Move element from its current position in the list to the head position in
 * the same list.
 *
 * Undefined behaviour if 'node' is not already part of the list.
 */
static inline void
dlist_move_head(dlist_head *head, dlist_node *node)
{
	/* fast path if it's already at the head */
	if (head->head.next == node)
		return;

	dlist_delete(node);
	dlist_push_head(head, node);

	dlist_check(head);
}

/*
 * Move element from its current position in the list to the tail position in
 * the same list.
 *
 * Undefined behaviour if 'node' is not already part of the list.
 */
static inline void
dlist_move_tail(dlist_head *head, dlist_node *node)
{
	/* fast path if it's already at the tail */
	if (head->head.prev == node)
		return;

	dlist_delete(node);
	dlist_push_tail(head, node);

	dlist_check(head);
}

/*
 * Check whether 'node' has a following node.
 * Caution: unreliable if 'node' is not in the list.
 */
static inline bool
dlist_has_next(const dlist_head *head, const dlist_node *node)
{
	return node->next != &head->head;
}

/*
 * Check whether 'node' has a preceding node.
 * Caution: unreliable if 'node' is not in the list.
 */
static inline bool
dlist_has_prev(const dlist_head *head, const dlist_node *node)
{
	return node->prev != &head->head;
}

/*
 * Check if node is detached. A node is only detached if it either has been
 * initialized with dlist_init_node(), or deleted with
 * dlist_delete_thoroughly() / dlist_delete_from_thoroughly() /
 * dclist_delete_from_thoroughly().
 */
static inline bool
dlist_node_is_detached(const dlist_node *node)
{
	Assert((node->next == NULL && node->prev == NULL) ||
		   (node->next != NULL && node->prev != NULL));

	return node->next == NULL;
}

/*
 * Return the next node in the list (there must be one).
 */
static inline dlist_node *
dlist_next_node(dlist_head *head, dlist_node *node)
{
	Assert(dlist_has_next(head, node));
	return node->next;
}

/*
 * Return previous node in the list (there must be one).
 */
static inline dlist_node *
dlist_prev_node(dlist_head *head, dlist_node *node)
{
	Assert(dlist_has_prev(head, node));
	return node->prev;
}

/* internal support function to get address of head element's struct */
static inline void *
dlist_head_element_off(dlist_head *head, size_t off)
{
	Assert(!dlist_is_empty(head));
	return (char *) head->head.next - off;
}

/*
 * Return the first node in the list (there must be one).
 */
static inline dlist_node *
dlist_head_node(dlist_head *head)
{
	return (dlist_node *) dlist_head_element_off(head, 0);
}

/* internal support function to get address of tail element's struct */
static inline void *
dlist_tail_element_off(dlist_head *head, size_t off)
{
	Assert(!dlist_is_empty(head));
	return (char *) head->head.prev - off;
}

/*
 * Return the last node in the list (there must be one).
 */
static inline dlist_node *
dlist_tail_node(dlist_head *head)
{
	return (dlist_node *) dlist_tail_element_off(head, 0);
}

/*
 * Return the containing struct of 'type' where 'membername' is the dlist_node
 * pointed at by 'ptr'.
 *
 * This is used to convert a dlist_node * back to its containing struct.
 */
#define dlist_container(type, membername, ptr)								\
	(AssertVariableIsOfTypeMacro(ptr, dlist_node *),						\
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) ((char *) (ptr) - offsetof(type, membername))))

/*
 * Return the address of the first element in the list.
 *
 * The list must not be empty.
 */
#define dlist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 (type *) dlist_head_element_off(lhead, offsetof(type, membername)))

/*
 * Return the address of the last element in the list.
 *
 * The list must not be empty.
 */
#define dlist_tail_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) dlist_tail_element_off(lhead, offsetof(type, membername))))

/*
 * Iterate through the list pointed at by 'lhead' storing the state in 'iter'.
 *
 * Access the current element with iter.cur.
 *
 * It is *not* allowed to manipulate the list during iteration.
 */
#define dlist_foreach(iter, lhead)											\
	for (AssertVariableIsOfTypeMacro(iter, dlist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->next ? (iter).end->next : (iter).end;		\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).cur->next)

/*
 * Iterate through the list pointed at by 'lhead' storing the state in 'iter'.
 *
 * Access the current element with iter.cur.
 *
 * Iterations using this are only allowed to change the list at the current
 * point of iteration. It is fine to delete the current node, but it is *not*
 * fine to insert or delete adjacent nodes.
 */
#define dlist_foreach_modify(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, dlist_mutable_iter),				\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->next ? (iter).end->next : (iter).end,		\
		 (iter).next = (iter).cur->next;									\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).next, (iter).next = (iter).cur->next)

/*
 * Iterate through the list in reverse order.
 *
 * It is *not* allowed to manipulate the list during iteration.
 */
#define dlist_reverse_foreach(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, dlist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->prev ? (iter).end->prev : (iter).end;		\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).cur->prev)

/* doubly-linked count list implementation */

/*
 * dclist_init
 *		Initialize a doubly linked count list.
 *
 * Previous state will be thrown away without any cleanup.
 */
static inline void
dclist_init(dclist_head *head)
{
	dlist_init(&head->dlist);
	head->count = 0;
}

/*
 * dclist_is_empty
 *		Returns true if the list is empty, otherwise false.
 */
static inline bool
dclist_is_empty(const dclist_head *head)
{
	Assert(dlist_is_empty(&head->dlist) == (head->count == 0));
	return (head->count == 0);
}

/*
 * dclist_push_head
 *		Insert a node at the beginning of the list.
 */
static inline void
dclist_push_head(dclist_head *head, dlist_node *node)
{
	if (head->dlist.head.next == NULL)	/* convert NULL header to circular */
		dclist_init(head);

	dlist_push_head(&head->dlist, node);
	head->count++;

	Assert(head->count > 0);	/* count overflow check */
}

/*
 * dclist_push_tail
 *		Insert a node at the end of the list.
 */
static inline void
dclist_push_tail(dclist_head *head, dlist_node *node)
{
	if (head->dlist.head.next == NULL)	/* convert NULL header to circular */
		dclist_init(head);

	dlist_push_tail(&head->dlist, node);
	head->count++;

	Assert(head->count > 0);	/* count overflow check */
}

/*
 * dclist_insert_after
 *		Insert a node after another *in the same list*
 *
 * Caution: 'after' must be a member of 'head'.
 */
static inline void
dclist_insert_after(dclist_head *head, dlist_node *after, dlist_node *node)
{
	dlist_member_check(&head->dlist, after);
	Assert(head->count > 0);	/* must be at least 1 already */

	dlist_insert_after(after, node);
	head->count++;

	Assert(head->count > 0);	/* count overflow check */
}

/*
 * dclist_insert_before
 *		Insert a node before another *in the same list*
 *
 * Caution: 'before' must be a member of 'head'.
 */
static inline void
dclist_insert_before(dclist_head *head, dlist_node *before, dlist_node *node)
{
	dlist_member_check(&head->dlist, before);
	Assert(head->count > 0);	/* must be at least 1 already */

	dlist_insert_before(before, node);
	head->count++;

	Assert(head->count > 0);	/* count overflow check */
}

/*
 * dclist_delete_from
 *		Deletes 'node' from 'head'.
 *
 * Caution: 'node' must be a member of 'head'.
 */
static inline void
dclist_delete_from(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	dlist_delete_from(&head->dlist, node);
	head->count--;
}

/*
 * Like dclist_delete_from(), but also sets next/prev to NULL to signal not
 * being in a list.
 */
static inline void
dclist_delete_from_thoroughly(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	dlist_delete_from_thoroughly(&head->dlist, node);
	head->count--;
}

/*
 * dclist_pop_head_node
 *		Remove and return the first node from a list (there must be one).
 */
static inline dlist_node *
dclist_pop_head_node(dclist_head *head)
{
	dlist_node *node;

	Assert(head->count > 0);

	node = dlist_pop_head_node(&head->dlist);
	head->count--;
	return node;
}

/*
 * dclist_move_head
 *		Move 'node' from its current position in the list to the head position
 *		in 'head'.
 *
 * Caution: 'node' must be a member of 'head'.
 */
static inline void
dclist_move_head(dclist_head *head, dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	dlist_move_head(&head->dlist, node);
}

/*
 * dclist_move_tail
 *		Move 'node' from its current position in the list to the tail position
 *		in 'head'.
 *
 * Caution: 'node' must be a member of 'head'.
 */
static inline void
dclist_move_tail(dclist_head *head, dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	dlist_move_tail(&head->dlist, node);
}

/*
 * dclist_has_next
 *		Check whether 'node' has a following node.
 *
 * Caution: 'node' must be a member of 'head'.
 */
static inline bool
dclist_has_next(const dclist_head *head, const dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	return dlist_has_next(&head->dlist, node);
}

/*
 * dclist_has_prev
 *		Check whether 'node' has a preceding node.
 *
 * Caution: 'node' must be a member of 'head'.
 */
static inline bool
dclist_has_prev(const dclist_head *head, const dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	return dlist_has_prev(&head->dlist, node);
}

/*
 * dclist_next_node
 *		Return the next node in the list (there must be one).
 */
static inline dlist_node *
dclist_next_node(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	return dlist_next_node(&head->dlist, node);
}

/*
 * dclist_prev_node
 *		Return the prev node in the list (there must be one).
 */
static inline dlist_node *
dclist_prev_node(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	return dlist_prev_node(&head->dlist, node);
}

/* internal support function to get address of head element's struct */
static inline void *
dclist_head_element_off(dclist_head *head, size_t off)
{
	Assert(!dclist_is_empty(head));

	return (char *) head->dlist.head.next - off;
}

/*
 * dclist_head_node
 *		Return the first node in the list (there must be one).
 */
static inline dlist_node *
dclist_head_node(dclist_head *head)
{
	Assert(head->count > 0);

	return (dlist_node *) dlist_head_element_off(&head->dlist, 0);
}

/* internal support function to get address of tail element's struct */
static inline void *
dclist_tail_element_off(dclist_head *head, size_t off)
{
	Assert(!dclist_is_empty(head));

	return (char *) head->dlist.head.prev - off;
}

/*
 * Return the last node in the list (there must be one).
 */
static inline dlist_node *
dclist_tail_node(dclist_head *head)
{
	Assert(head->count > 0);

	return (dlist_node *) dlist_tail_element_off(&head->dlist, 0);
}

/*
 * dclist_count
 *		Returns the stored number of entries in 'head'
 */
static inline uint32
dclist_count(const dclist_head *head)
{
	Assert(dlist_is_empty(&head->dlist) == (head->count == 0));

	return head->count;
}

/*
 * Return the containing struct of 'type' where 'membername' is the dlist_node
 * pointed at by 'ptr'.
 *
 * This is used to convert a dlist_node * back to its containing struct.
 *
 * Note: This is effectively just the same as dlist_container, so reuse that.
 */
#define dclist_container(type, membername, ptr) \
		dlist_container(type, membername, ptr)

 /*
  * Return the address of the first element in the list.
  *
  * The list must not be empty.
  */
#define dclist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 (type *) dclist_head_element_off(lhead, offsetof(type, membername)))

 /*
  * Return the address of the last element in the list.
  *
  * The list must not be empty.
  */
#define dclist_tail_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) dclist_tail_element_off(lhead, offsetof(type, membername))))


/* Iterators for dclists */
#define dclist_foreach(iter, lhead) \
	dlist_foreach(iter, &((lhead)->dlist))

#define dclist_foreach_modify(iter, lhead) \
	dlist_foreach_modify(iter, &((lhead)->dlist))

#define dclist_reverse_foreach(iter, lhead) \
	dlist_reverse_foreach(iter, &((lhead)->dlist))

/* singly linked list implementation */

/*
 * Initialize a singly linked list.
 * Previous state will be thrown away without any cleanup.
 */
static inline void
slist_init(slist_head *head)
{
	head->head.next = NULL;
}

/*
 * Is the list empty?
 */
static inline bool
slist_is_empty(const slist_head *head)
{
	slist_check(head);

	return head->head.next == NULL;
}

/*
 * Insert a node at the beginning of the list.
 */
static inline void
slist_push_head(slist_head *head, slist_node *node)
{
	node->next = head->head.next;
	head->head.next = node;

	slist_check(head);
}

/*
 * Insert a node after another *in the same list*
 */
static inline void
slist_insert_after(slist_node *after, slist_node *node)
{
	node->next = after->next;
	after->next = node;
}

/*
 * Remove and return the first node from a list (there must be one).
 */
static inline slist_node *
slist_pop_head_node(slist_head *head)
{
	slist_node *node;

	Assert(!slist_is_empty(head));
	node = head->head.next;
	head->head.next = node->next;
	slist_check(head);
	return node;
}

/*
 * Check whether 'node' has a following node.
 */
static inline bool
slist_has_next(const slist_head *head, const slist_node *node)
{
	slist_check(head);

	return node->next != NULL;
}

/*
 * Return the next node in the list (there must be one).
 */
static inline slist_node *
slist_next_node(slist_head *head, slist_node *node)
{
	Assert(slist_has_next(head, node));
	return node->next;
}

/* internal support function to get address of head element's struct */
static inline void *
slist_head_element_off(slist_head *head, size_t off)
{
	Assert(!slist_is_empty(head));
	return (char *) head->head.next - off;
}

/*
 * Return the first node in the list (there must be one).
 */
static inline slist_node *
slist_head_node(slist_head *head)
{
	return (slist_node *) slist_head_element_off(head, 0);
}

/*
 * Delete the list element the iterator currently points to.
 *
 * Caution: this modifies iter->cur, so don't use that again in the current
 * loop iteration.
 */
static inline void
slist_delete_current(slist_mutable_iter *iter)
{
	/*
	 * Update previous element's forward link.  If the iteration is at the
	 * first list element, iter->prev will point to the list header's "head"
	 * field, so we don't need a special case for that.
	 */
	iter->prev->next = iter->next;

	/*
	 * Reset cur to prev, so that prev will continue to point to the prior
	 * valid list element after slist_foreach_modify() advances to the next.
	 */
	iter->cur = iter->prev;
}

/*
 * Return the containing struct of 'type' where 'membername' is the slist_node
 * pointed at by 'ptr'.
 *
 * This is used to convert a slist_node * back to its containing struct.
 */
#define slist_container(type, membername, ptr)								\
	(AssertVariableIsOfTypeMacro(ptr, slist_node *),						\
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, slist_node),	\
	 ((type *) ((char *) (ptr) - offsetof(type, membername))))

/*
 * Return the address of the first element in the list.
 *
 * The list must not be empty.
 */
#define slist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, slist_node),	\
	 (type *) slist_head_element_off(lhead, offsetof(type, membername)))

/*
 * Iterate through the list pointed at by 'lhead' storing the state in 'iter'.
 *
 * Access the current element with iter.cur.
 *
 * It's allowed to modify the list while iterating, with the exception of
 * deleting the iterator's current node; deletion of that node requires
 * care if the iteration is to be continued afterward.  (Doing so and also
 * deleting or inserting adjacent list elements might misbehave; also, if
 * the user frees the current node's storage, continuing the iteration is
 * not safe.)
 */
#define slist_foreach(iter, lhead)											\
	for (AssertVariableIsOfTypeMacro(iter, slist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, slist_head *),					\
		 (iter).cur = (lhead)->head.next;									\
		 (iter).cur != NULL;												\
		 (iter).cur = (iter).cur->next)

/*
 * Iterate through the list pointed at by 'lhead' storing the state in 'iter'.
 *
 * Access the current element with iter.cur.
 *
 * The only list modification allowed while iterating is to remove the current
 * node via slist_delete_current() (*not* slist_delete()).  Insertion or
 * deletion of nodes adjacent to the current node would misbehave.
 */
#define slist_foreach_modify(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, slist_mutable_iter),				\
		 AssertVariableIsOfTypeMacro(lhead, slist_head *),					\
		 (iter).prev = &(lhead)->head,										\
		 (iter).cur = (iter).prev->next,									\
		 (iter).next = (iter).cur ? (iter).cur->next : NULL;				\
		 (iter).cur != NULL;												\
		 (iter).prev = (iter).cur,											\
		 (iter).cur = (iter).next,											\
		 (iter).next = (iter).next ? (iter).next->next : NULL)

#endif							/* ILIST_H */
