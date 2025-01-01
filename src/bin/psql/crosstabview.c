/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/crosstabview.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/int.h"
#include "common/logging.h"
#include "crosstabview.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"

/*
 * Value/position from the resultset that goes into the horizontal or vertical
 * crosstabview header.
 */
typedef struct _pivot_field
{
	/*
	 * Pointer obtained from PQgetvalue() for colV or colH. Each distinct
	 * value becomes an entry in the vertical header (colV), or horizontal
	 * header (colH). A Null value is represented by a NULL pointer.
	 */
	char	   *name;

	/*
	 * When a sort is requested on an alternative column, this holds
	 * PQgetvalue() for the sort column corresponding to <name>. If <name>
	 * appear multiple times, it's the first value in the order of the results
	 * that is kept. A Null value is represented by a NULL pointer.
	 */
	char	   *sort_value;

	/*
	 * Rank of this value, starting at 0. Initially, it's the relative
	 * position of the first appearance of <name> in the resultset. For
	 * example, if successive rows contain B,A,C,A,D then it's B:0,A:1,C:2,D:3
	 * When a sort column is specified, ranks get updated in a final pass to
	 * reflect the desired order.
	 */
	int			rank;
} pivot_field;

/* Node in avl_tree */
typedef struct _avl_node
{
	/* Node contents */
	pivot_field field;

	/*
	 * Height of this node in the tree (number of nodes on the longest path to
	 * a leaf).
	 */
	int			height;

	/*
	 * Child nodes. [0] points to left subtree, [1] to right subtree. Never
	 * NULL, points to the empty node avl_tree.end when no left or right
	 * value.
	 */
	struct _avl_node *children[2];
} avl_node;

/*
 * Control structure for the AVL tree (binary search tree kept
 * balanced with the AVL algorithm)
 */
typedef struct _avl_tree
{
	int			count;			/* Total number of nodes */
	avl_node   *root;			/* root of the tree */
	avl_node   *end;			/* Immutable dereferenceable empty tree */
} avl_tree;


static bool printCrosstab(const PGresult *result,
						  int num_columns, pivot_field *piv_columns, int field_for_columns,
						  int num_rows, pivot_field *piv_rows, int field_for_rows,
						  int field_for_data);
static void avlInit(avl_tree *tree);
static void avlMergeValue(avl_tree *tree, char *name, char *sort_value);
static int	avlCollectFields(avl_tree *tree, avl_node *node,
							 pivot_field *fields, int idx);
static void avlFree(avl_tree *tree, avl_node *node);
static void rankSort(int num_columns, pivot_field *piv_columns);
static int	indexOfColumn(char *arg, const PGresult *res);
static int	pivotFieldCompare(const void *a, const void *b);
static int	rankCompare(const void *a, const void *b);


/*
 * Main entry point to this module.
 *
 * Process the data from *res according to the options in pset (global),
 * to generate the horizontal and vertical headers contents,
 * then call printCrosstab() for the actual output.
 */
bool
PrintResultInCrosstab(const PGresult *res)
{
	bool		retval = false;
	avl_tree	piv_columns;
	avl_tree	piv_rows;
	pivot_field *array_columns = NULL;
	pivot_field *array_rows = NULL;
	int			num_columns = 0;
	int			num_rows = 0;
	int			field_for_rows;
	int			field_for_columns;
	int			field_for_data;
	int			sort_field_for_columns;
	int			rn;

	avlInit(&piv_rows);
	avlInit(&piv_columns);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("\\crosstabview: statement did not return a result set");
		goto error_return;
	}

	if (PQnfields(res) < 3)
	{
		pg_log_error("\\crosstabview: query must return at least three columns");
		goto error_return;
	}

	/* Process first optional arg (vertical header column) */
	if (pset.ctv_args[0] == NULL)
		field_for_rows = 0;
	else
	{
		field_for_rows = indexOfColumn(pset.ctv_args[0], res);
		if (field_for_rows < 0)
			goto error_return;
	}

	/* Process second optional arg (horizontal header column) */
	if (pset.ctv_args[1] == NULL)
		field_for_columns = 1;
	else
	{
		field_for_columns = indexOfColumn(pset.ctv_args[1], res);
		if (field_for_columns < 0)
			goto error_return;
	}

	/* Insist that header columns be distinct */
	if (field_for_columns == field_for_rows)
	{
		pg_log_error("\\crosstabview: vertical and horizontal headers must be different columns");
		goto error_return;
	}

	/* Process third optional arg (data column) */
	if (pset.ctv_args[2] == NULL)
	{
		int			i;

		/*
		 * If the data column was not specified, we search for the one not
		 * used as either vertical or horizontal headers.  Must be exactly
		 * three columns, or this won't be unique.
		 */
		if (PQnfields(res) != 3)
		{
			pg_log_error("\\crosstabview: data column must be specified when query returns more than three columns");
			goto error_return;
		}

		field_for_data = -1;
		for (i = 0; i < PQnfields(res); i++)
		{
			if (i != field_for_rows && i != field_for_columns)
			{
				field_for_data = i;
				break;
			}
		}
		Assert(field_for_data >= 0);
	}
	else
	{
		field_for_data = indexOfColumn(pset.ctv_args[2], res);
		if (field_for_data < 0)
			goto error_return;
	}

	/* Process fourth optional arg (horizontal header sort column) */
	if (pset.ctv_args[3] == NULL)
		sort_field_for_columns = -1;	/* no sort column */
	else
	{
		sort_field_for_columns = indexOfColumn(pset.ctv_args[3], res);
		if (sort_field_for_columns < 0)
			goto error_return;
	}

	/*
	 * First part: accumulate the names that go into the vertical and
	 * horizontal headers, each into an AVL binary tree to build the set of
	 * DISTINCT values.
	 */

	for (rn = 0; rn < PQntuples(res); rn++)
	{
		char	   *val;
		char	   *val1;

		/* horizontal */
		val = PQgetisnull(res, rn, field_for_columns) ? NULL :
			PQgetvalue(res, rn, field_for_columns);
		val1 = NULL;

		if (sort_field_for_columns >= 0 &&
			!PQgetisnull(res, rn, sort_field_for_columns))
			val1 = PQgetvalue(res, rn, sort_field_for_columns);

		avlMergeValue(&piv_columns, val, val1);

		if (piv_columns.count > CROSSTABVIEW_MAX_COLUMNS)
		{
			pg_log_error("\\crosstabview: maximum number of columns (%d) exceeded",
						 CROSSTABVIEW_MAX_COLUMNS);
			goto error_return;
		}

		/* vertical */
		val = PQgetisnull(res, rn, field_for_rows) ? NULL :
			PQgetvalue(res, rn, field_for_rows);

		avlMergeValue(&piv_rows, val, NULL);
	}

	/*
	 * Second part: Generate sorted arrays from the AVL trees.
	 */

	num_columns = piv_columns.count;
	num_rows = piv_rows.count;

	array_columns = (pivot_field *)
		pg_malloc(sizeof(pivot_field) * num_columns);

	array_rows = (pivot_field *)
		pg_malloc(sizeof(pivot_field) * num_rows);

	avlCollectFields(&piv_columns, piv_columns.root, array_columns, 0);
	avlCollectFields(&piv_rows, piv_rows.root, array_rows, 0);

	/*
	 * Third part: optionally, process the ranking data for the horizontal
	 * header
	 */
	if (sort_field_for_columns >= 0)
		rankSort(num_columns, array_columns);

	/*
	 * Fourth part: print the crosstab'ed result.
	 */
	retval = printCrosstab(res,
						   num_columns, array_columns, field_for_columns,
						   num_rows, array_rows, field_for_rows,
						   field_for_data);

error_return:
	avlFree(&piv_columns, piv_columns.root);
	avlFree(&piv_rows, piv_rows.root);
	pg_free(array_columns);
	pg_free(array_rows);

	return retval;
}

/*
 * Output the pivoted resultset with the printTable* functions.  Return true
 * if successful, false otherwise.
 */
static bool
printCrosstab(const PGresult *result,
			  int num_columns, pivot_field *piv_columns, int field_for_columns,
			  int num_rows, pivot_field *piv_rows, int field_for_rows,
			  int field_for_data)
{
	printQueryOpt popt = pset.popt;
	printTableContent cont;
	int			i,
				rn;
	char		col_align;
	int		   *horiz_map;
	bool		retval = false;

	printTableInit(&cont, &popt.topt, popt.title, num_columns + 1, num_rows);

	/* Step 1: set target column names (horizontal header) */

	/* The name of the first column is kept unchanged by the pivoting */
	printTableAddHeader(&cont,
						PQfname(result, field_for_rows),
						false,
						column_type_alignment(PQftype(result,
													  field_for_rows)));

	/*
	 * To iterate over piv_columns[] by piv_columns[].rank, create a reverse
	 * map associating each piv_columns[].rank to its index in piv_columns.
	 * This avoids an O(N^2) loop later.
	 */
	horiz_map = (int *) pg_malloc(sizeof(int) * num_columns);
	for (i = 0; i < num_columns; i++)
		horiz_map[piv_columns[i].rank] = i;

	/*
	 * The display alignment depends on its PQftype().
	 */
	col_align = column_type_alignment(PQftype(result, field_for_data));

	for (i = 0; i < num_columns; i++)
	{
		char	   *colname;

		colname = piv_columns[horiz_map[i]].name ?
			piv_columns[horiz_map[i]].name :
			(popt.nullPrint ? popt.nullPrint : "");

		printTableAddHeader(&cont, colname, false, col_align);
	}
	pg_free(horiz_map);

	/* Step 2: set row names in the first output column (vertical header) */
	for (i = 0; i < num_rows; i++)
	{
		int			k = piv_rows[i].rank;

		cont.cells[k * (num_columns + 1)] = piv_rows[i].name ?
			piv_rows[i].name :
			(popt.nullPrint ? popt.nullPrint : "");
	}
	cont.cellsadded = num_rows * (num_columns + 1);

	/*
	 * Step 3: fill in the content cells.
	 */
	for (rn = 0; rn < PQntuples(result); rn++)
	{
		int			row_number;
		int			col_number;
		pivot_field *rp,
				   *cp;
		pivot_field elt;

		/* Find target row */
		if (!PQgetisnull(result, rn, field_for_rows))
			elt.name = PQgetvalue(result, rn, field_for_rows);
		else
			elt.name = NULL;
		rp = (pivot_field *) bsearch(&elt,
									 piv_rows,
									 num_rows,
									 sizeof(pivot_field),
									 pivotFieldCompare);
		Assert(rp != NULL);
		row_number = rp->rank;

		/* Find target column */
		if (!PQgetisnull(result, rn, field_for_columns))
			elt.name = PQgetvalue(result, rn, field_for_columns);
		else
			elt.name = NULL;

		cp = (pivot_field *) bsearch(&elt,
									 piv_columns,
									 num_columns,
									 sizeof(pivot_field),
									 pivotFieldCompare);
		Assert(cp != NULL);
		col_number = cp->rank;

		/* Place value into cell */
		if (col_number >= 0 && row_number >= 0)
		{
			int			idx;

			/* index into the cont.cells array */
			idx = 1 + col_number + row_number * (num_columns + 1);

			/*
			 * If the cell already contains a value, raise an error.
			 */
			if (cont.cells[idx] != NULL)
			{
				pg_log_error("\\crosstabview: query result contains multiple data values for row \"%s\", column \"%s\"",
							 rp->name ? rp->name :
							 (popt.nullPrint ? popt.nullPrint : "(null)"),
							 cp->name ? cp->name :
							 (popt.nullPrint ? popt.nullPrint : "(null)"));
				goto error;
			}

			cont.cells[idx] = !PQgetisnull(result, rn, field_for_data) ?
				PQgetvalue(result, rn, field_for_data) :
				(popt.nullPrint ? popt.nullPrint : "");
		}
	}

	/*
	 * The non-initialized cells must be set to an empty string for the print
	 * functions
	 */
	for (i = 0; i < cont.cellsadded; i++)
	{
		if (cont.cells[i] == NULL)
			cont.cells[i] = "";
	}

	printTable(&cont, pset.queryFout, false, pset.logfile);
	retval = true;

error:
	printTableCleanup(&cont);

	return retval;
}

/*
 * The avl* functions below provide a minimalistic implementation of AVL binary
 * trees, to efficiently collect the distinct values that will form the horizontal
 * and vertical headers. It only supports adding new values, no removal or even
 * search.
 */
static void
avlInit(avl_tree *tree)
{
	tree->end = (avl_node *) pg_malloc0(sizeof(avl_node));
	tree->end->children[0] = tree->end->children[1] = tree->end;
	tree->count = 0;
	tree->root = tree->end;
}

/* Deallocate recursively an AVL tree, starting from node */
static void
avlFree(avl_tree *tree, avl_node *node)
{
	if (node->children[0] != tree->end)
	{
		avlFree(tree, node->children[0]);
		pg_free(node->children[0]);
	}
	if (node->children[1] != tree->end)
	{
		avlFree(tree, node->children[1]);
		pg_free(node->children[1]);
	}
	if (node == tree->root)
	{
		/* free the root separately as it's not child of anything */
		if (node != tree->end)
			pg_free(node);
		/* free the tree->end struct only once and when all else is freed */
		pg_free(tree->end);
	}
}

/* Set the height to 1 plus the greatest of left and right heights */
static void
avlUpdateHeight(avl_node *n)
{
	n->height = 1 + (n->children[0]->height > n->children[1]->height ?
					 n->children[0]->height :
					 n->children[1]->height);
}

/* Rotate a subtree left (dir=0) or right (dir=1). Not recursive */
static avl_node *
avlRotate(avl_node **current, int dir)
{
	avl_node   *before = *current;
	avl_node   *after = (*current)->children[dir];

	*current = after;
	before->children[dir] = after->children[!dir];
	avlUpdateHeight(before);
	after->children[!dir] = before;

	return after;
}

static int
avlBalance(avl_node *n)
{
	return n->children[0]->height - n->children[1]->height;
}

/*
 * After an insertion, possibly rebalance the tree so that the left and right
 * node heights don't differ by more than 1.
 * May update *node.
 */
static void
avlAdjustBalance(avl_tree *tree, avl_node **node)
{
	avl_node   *current = *node;
	int			b = avlBalance(current) / 2;

	if (b != 0)
	{
		int			dir = (1 - b) / 2;

		if (avlBalance(current->children[dir]) == -b)
			avlRotate(&current->children[dir], !dir);
		current = avlRotate(node, dir);
	}
	if (current != tree->end)
		avlUpdateHeight(current);
}

/*
 * Insert a new value/field, starting from *node, reaching the correct position
 * in the tree by recursion.  Possibly rebalance the tree and possibly update
 * *node.  Do nothing if the value is already present in the tree.
 */
static void
avlInsertNode(avl_tree *tree, avl_node **node, pivot_field field)
{
	avl_node   *current = *node;

	if (current == tree->end)
	{
		avl_node   *new_node = (avl_node *)
			pg_malloc(sizeof(avl_node));

		new_node->height = 1;
		new_node->field = field;
		new_node->children[0] = new_node->children[1] = tree->end;
		tree->count++;
		*node = new_node;
	}
	else
	{
		int			cmp = pivotFieldCompare(&field, &current->field);

		if (cmp != 0)
		{
			avlInsertNode(tree,
						  cmp > 0 ? &current->children[1] : &current->children[0],
						  field);
			avlAdjustBalance(tree, node);
		}
	}
}

/* Insert the value into the AVL tree, if it does not preexist */
static void
avlMergeValue(avl_tree *tree, char *name, char *sort_value)
{
	pivot_field field;

	field.name = name;
	field.rank = tree->count;
	field.sort_value = sort_value;
	avlInsertNode(tree, &tree->root, field);
}

/*
 * Recursively extract node values into the names array, in sorted order with a
 * left-to-right tree traversal.
 * Return the next candidate offset to write into the names array.
 * fields[] must be preallocated to hold tree->count entries
 */
static int
avlCollectFields(avl_tree *tree, avl_node *node, pivot_field *fields, int idx)
{
	if (node == tree->end)
		return idx;

	idx = avlCollectFields(tree, node->children[0], fields, idx);
	fields[idx] = node->field;
	return avlCollectFields(tree, node->children[1], fields, idx + 1);
}

static void
rankSort(int num_columns, pivot_field *piv_columns)
{
	int		   *hmap;			/* [[offset in piv_columns, rank], ...for
								 * every header entry] */
	int			i;

	hmap = (int *) pg_malloc(sizeof(int) * num_columns * 2);
	for (i = 0; i < num_columns; i++)
	{
		char	   *val = piv_columns[i].sort_value;

		/* ranking information is valid if non null and matches /^-?\d+$/ */
		if (val &&
			((*val == '-' &&
			  strspn(val + 1, "0123456789") == strlen(val + 1)) ||
			 strspn(val, "0123456789") == strlen(val)))
		{
			hmap[i * 2] = atoi(val);
			hmap[i * 2 + 1] = i;
		}
		else
		{
			/* invalid rank information ignored (equivalent to rank 0) */
			hmap[i * 2] = 0;
			hmap[i * 2 + 1] = i;
		}
	}

	qsort(hmap, num_columns, sizeof(int) * 2, rankCompare);

	for (i = 0; i < num_columns; i++)
	{
		piv_columns[hmap[i * 2 + 1]].rank = i;
	}

	pg_free(hmap);
}

/*
 * Look up a column reference, which can be either:
 * - a number from 1 to PQnfields(res)
 * - a column name matching one of PQfname(res,...)
 *
 * Returns zero-based column number, or -1 if not found or ambiguous.
 *
 * Note: may modify contents of "arg" string.
 */
static int
indexOfColumn(char *arg, const PGresult *res)
{
	int			idx;

	if (arg[0] && strspn(arg, "0123456789") == strlen(arg))
	{
		/* if arg contains only digits, it's a column number */
		idx = atoi(arg) - 1;
		if (idx < 0 || idx >= PQnfields(res))
		{
			pg_log_error("\\crosstabview: column number %d is out of range 1..%d",
						 idx + 1, PQnfields(res));
			return -1;
		}
	}
	else
	{
		int			i;

		/*
		 * Dequote and downcase the column name.  By checking for all-digits
		 * before doing this, we can ensure that a quoted name is treated as a
		 * name even if it's all digits.
		 */
		dequote_downcase_identifier(arg, true, pset.encoding);

		/* Now look for match(es) among res' column names */
		idx = -1;
		for (i = 0; i < PQnfields(res); i++)
		{
			if (strcmp(arg, PQfname(res, i)) == 0)
			{
				if (idx >= 0)
				{
					/* another idx was already found for the same name */
					pg_log_error("\\crosstabview: ambiguous column name: \"%s\"", arg);
					return -1;
				}
				idx = i;
			}
		}
		if (idx == -1)
		{
			pg_log_error("\\crosstabview: column name not found: \"%s\"", arg);
			return -1;
		}
	}

	return idx;
}

/*
 * Value comparator for vertical and horizontal headers
 * used for deduplication only.
 * - null values are considered equal
 * - non-null < null
 * - non-null values are compared with strcmp()
 */
static int
pivotFieldCompare(const void *a, const void *b)
{
	const pivot_field *pa = (const pivot_field *) a;
	const pivot_field *pb = (const pivot_field *) b;

	/* test null values */
	if (!pb->name)
		return pa->name ? -1 : 0;
	else if (!pa->name)
		return 1;

	/* non-null values */
	return strcmp(pa->name, pb->name);
}

static int
rankCompare(const void *a, const void *b)
{
	return pg_cmp_s32(*(const int *) a, *(const int *) b);
}
