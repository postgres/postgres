/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/crosstabview.c
 */
#include "postgres_fe.h"

#include <string.h>

#include "common.h"
#include "crosstabview.h"
#include "pqexpbuffer.h"
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


static bool printCrosstab(const PGresult *results,
			  int num_columns, pivot_field *piv_columns, int field_for_columns,
			  int num_rows, pivot_field *piv_rows, int field_for_rows,
			  int field_for_data);
static int parseColumnRefs(const char *arg, const PGresult *res,
				int **col_numbers,
				int max_columns, char separator);
static void avlInit(avl_tree *tree);
static void avlMergeValue(avl_tree *tree, char *name, char *sort_value);
static int avlCollectFields(avl_tree *tree, avl_node *node,
				 pivot_field *fields, int idx);
static void avlFree(avl_tree *tree, avl_node *node);
static void rankSort(int num_columns, pivot_field *piv_columns);
static int	indexOfColumn(const char *arg, const PGresult *res);
static int	pivotFieldCompare(const void *a, const void *b);
static int	rankCompare(const void *a, const void *b);


/*
 * Main entry point to this module.
 *
 * Process the data from *res according the display options in pset (global),
 * to generate the horizontal and vertical headers contents,
 * then call printCrosstab() for the actual output.
 */
bool
PrintResultsInCrosstab(const PGresult *res)
{
	char	   *opt_field_for_rows = pset.ctv_col_V;
	char	   *opt_field_for_columns = pset.ctv_col_H;
	char	   *opt_field_for_data = pset.ctv_col_D;
	int			rn;
	avl_tree	piv_columns;
	avl_tree	piv_rows;
	pivot_field *array_columns = NULL;
	pivot_field *array_rows = NULL;
	int			num_columns = 0;
	int			num_rows = 0;
	int		   *colsV = NULL,
			   *colsH = NULL,
			   *colsD = NULL;
	int			n;
	int			field_for_columns;
	int			sort_field_for_columns = -1;
	int			field_for_rows;
	int			field_for_data = -1;
	bool		retval = false;

	avlInit(&piv_rows);
	avlInit(&piv_columns);

	if (res == NULL)
	{
		psql_error(_("No result\n"));
		goto error_return;
	}

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		psql_error(_("The query must return results to be shown in crosstab\n"));
		goto error_return;
	}

	if (opt_field_for_rows && !opt_field_for_columns)
	{
		psql_error(_("A second column must be specified for the horizontal header\n"));
		goto error_return;
	}

	if (PQnfields(res) <= 2)
	{
		psql_error(_("The query must return at least two columns to be shown in crosstab\n"));
		goto error_return;
	}

	/*
	 * Arguments processing for the vertical header (1st arg) displayed in the
	 * left-most column. Only a reference to a field is accepted (no sort
	 * column).
	 */

	if (opt_field_for_rows == NULL)
	{
		field_for_rows = 0;
	}
	else
	{
		n = parseColumnRefs(opt_field_for_rows, res, &colsV, 1, ':');
		if (n != 1)
			goto error_return;
		field_for_rows = colsV[0];
	}

	if (field_for_rows < 0)
		goto error_return;

	/*----------
	 * Arguments processing for the horizontal header (2nd arg)
	 * (pivoted column that gets displayed as the first row).
	 * Determine:
	 * - the field number for the horizontal header column
	 * - the field number of the associated sort column, if any
	 */

	if (opt_field_for_columns == NULL)
		field_for_columns = 1;
	else
	{
		n = parseColumnRefs(opt_field_for_columns, res, &colsH, 2, ':');
		if (n <= 0)
			goto error_return;
		if (n == 1)
			field_for_columns = colsH[0];
		else
		{
			field_for_columns = colsH[0];
			sort_field_for_columns = colsH[1];
		}

		if (field_for_columns < 0)
			goto error_return;
	}

	if (field_for_columns == field_for_rows)
	{
		psql_error(_("The same column cannot be used for both vertical and horizontal headers\n"));
		goto error_return;
	}

	/*
	 * Arguments processing for the data columns (3rd arg).  Determine the
	 * column to display in the grid.
	 */
	if (opt_field_for_data == NULL)
	{
		int		i;

		/*
		 * If the data column was not specified, we search for the one not
		 * used as either vertical or horizontal headers.  If the result has
		 * more than three columns, raise an error.
		 */
		if (PQnfields(res) > 3)
		{
			psql_error(_("Data column must be specified when the result set has more than three columns\n"));
			goto error_return;
		}

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
		int		num_fields;

		/* If a field was given, find out what it is.  Only one is allowed. */
		num_fields = parseColumnRefs(opt_field_for_data, res, &colsD, 1, ',');
		if (num_fields < 1)
			goto error_return;
		field_for_data = colsD[0];
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
			psql_error(_("Maximum number of columns (%d) exceeded\n"),
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
	 * Fourth part: print the crosstab'ed results.
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
	pg_free(colsV);
	pg_free(colsH);
	pg_free(colsD);

	return retval;
}

/*
 * Output the pivoted resultset with the printTable* functions.  Return true
 * if successful, false otherwise.
 */
static bool
printCrosstab(const PGresult *results,
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
						PQfname(results, field_for_rows),
						false,
						column_type_alignment(PQftype(results,
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
	col_align = column_type_alignment(PQftype(results, field_for_data));

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
	for (rn = 0; rn < PQntuples(results); rn++)
	{
		int			row_number;
		int			col_number;
		pivot_field *p;
		pivot_field elt;

		/* Find target row */
		if (!PQgetisnull(results, rn, field_for_rows))
			elt.name = PQgetvalue(results, rn, field_for_rows);
		else
			elt.name = NULL;
		p = (pivot_field *) bsearch(&elt,
									piv_rows,
									num_rows,
									sizeof(pivot_field),
									pivotFieldCompare);
		Assert(p != NULL);
		row_number = p->rank;

		/* Find target column */
		if (!PQgetisnull(results, rn, field_for_columns))
			elt.name = PQgetvalue(results, rn, field_for_columns);
		else
			elt.name = NULL;

		p = (pivot_field *) bsearch(&elt,
									piv_columns,
									num_columns,
									sizeof(pivot_field),
									pivotFieldCompare);
		Assert(p != NULL);
		col_number = p->rank;

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
				psql_error(_("data cell already contains a value: (row: \"%s\", column: \"%s\")\n"),
							 piv_rows[row_number].name ? piv_rows[row_number].name :
							 popt.nullPrint ? popt.nullPrint : "(null)",
							 piv_columns[col_number].name ? piv_columns[col_number].name :
							 popt.nullPrint ? popt.nullPrint : "(null)");
				goto error;
			}

			cont.cells[idx] = !PQgetisnull(results, rn, field_for_data) ?
				PQgetvalue(results, rn, field_for_data) :
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
 * Parse "arg", which is a string of column IDs separated by "separator".
 *
 * Each column ID can be:
 * - a number from 1 to PQnfields(res)
 * - an unquoted column name matching (case insensitively) one of PQfname(res,...)
 * - a quoted column name matching (case sensitively) one of PQfname(res,...)
 *
 * If max_columns > 0, it is the max number of column IDs allowed.
 *
 * On success, return number of column IDs found (possibly 0), and return a
 * malloc'd array of the matching column numbers of "res" into *col_numbers.
 *
 * On failure, return -1 and set *col_numbers to NULL.
 */
static int
parseColumnRefs(const char *arg,
				const PGresult *res,
				int **col_numbers,
				int max_columns,
				char separator)
{
	const char *p = arg;
	char		c;
	int			num_cols = 0;

	*col_numbers = NULL;
	while ((c = *p) != '\0')
	{
		const char *field_start = p;
		bool		quoted_field = false;

		/* first char */
		if (c == '"')
		{
			quoted_field = true;
			p++;
		}

		while ((c = *p) != '\0')
		{
			if (c == separator && !quoted_field)
				break;
			if (c == '"')		/* end of field or embedded double quote */
			{
				p++;
				if (*p == '"')
				{
					if (quoted_field)
					{
						p++;
						continue;
					}
				}
				else if (quoted_field && *p == separator)
					break;
			}
			if (*p)
				p += PQmblen(p, pset.encoding);
		}

		if (p != field_start)
		{
			char   *col_name;
			int		col_num;

			/* enforce max_columns limit */
			if (max_columns > 0 && num_cols == max_columns)
			{
				psql_error(_("No more than %d column references expected\n"),
						   max_columns);
				goto errfail;
			}
			/* look up the column and add its index into *col_numbers */
			col_name = pg_malloc(p - field_start + 1);
			memcpy(col_name, field_start, p - field_start);
			col_name[p - field_start] = '\0';
			col_num = indexOfColumn(col_name, res);
			pg_free(col_name);
			if (col_num < 0)
				goto errfail;
			*col_numbers = (int *) pg_realloc(*col_numbers,
											  (num_cols + 1) * sizeof(int));
			(*col_numbers)[num_cols++] = col_num;
		}
		else
		{
			psql_error(_("Empty column reference\n"));
			goto errfail;
		}

		if (*p)
			p += PQmblen(p, pset.encoding);
	}
	return num_cols;

errfail:
	pg_free(*col_numbers);
	*col_numbers = NULL;
	return -1;
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
 * Compare a user-supplied argument against a field name obtained by PQfname(),
 * which is already case-folded.
 * If arg is not enclosed in double quotes, pg_strcasecmp applies, otherwise
 * do a case-sensitive comparison with these rules:
 * - double quotes enclosing 'arg' are filtered out
 * - double quotes inside 'arg' are expected to be doubled
 */
static bool
fieldNameEquals(const char *arg, const char *fieldname)
{
	const char *p = arg;
	const char *f = fieldname;
	char		c;

	if (*p++ != '"')
		return (pg_strcasecmp(arg, fieldname) == 0);

	while ((c = *p++))
	{
		if (c == '"')
		{
			if (*p == '"')
				p++;			/* skip second quote and continue */
			else if (*p == '\0')
				return (*f == '\0');	/* p is shorter than f, or is
										 * identical */
		}
		if (*f == '\0')
			return false;		/* f is shorter than p */
		if (c != *f)			/* found one byte that differs */
			return false;
		f++;
	}
	return (*f == '\0');
}

/*
 * arg can be a number or a column name, possibly quoted (like in an ORDER BY clause)
 * Returns:
 *	on success, the 0-based index of the column
 *	or -1 if the column number or name is not found in the result's structure,
 *		  or if it's ambiguous (arg corresponding to several columns)
 */
static int
indexOfColumn(const char *arg, const PGresult *res)
{
	int			idx;

	if (strspn(arg, "0123456789") == strlen(arg))
	{
		/* if arg contains only digits, it's a column number */
		idx = atoi(arg) - 1;
		if (idx < 0 || idx >= PQnfields(res))
		{
			psql_error(_("Invalid column number: %s\n"), arg);
			return -1;
		}
	}
	else
	{
		int			i;

		idx = -1;
		for (i = 0; i < PQnfields(res); i++)
		{
			if (fieldNameEquals(arg, PQfname(res, i)))
			{
				if (idx >= 0)
				{
					/* if another idx was already found for the same name */
					psql_error(_("Ambiguous column name: %s\n"), arg);
					return -1;
				}
				idx = i;
			}
		}
		if (idx == -1)
		{
			psql_error(_("Invalid column name: %s\n"), arg);
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
	pivot_field *pa = (pivot_field *) a;
	pivot_field *pb = (pivot_field *) b;

	/* test null values */
	if (!pb->name)
		return pa->name ? -1 : 0;
	else if (!pa->name)
		return 1;

	/* non-null values */
	return strcmp(((pivot_field *) a)->name,
				  ((pivot_field *) b)->name);
}

static int
rankCompare(const void *a, const void *b)
{
	return *((int *) a) - *((int *) b);
}
