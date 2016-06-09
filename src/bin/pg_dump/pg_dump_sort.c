/*-------------------------------------------------------------------------
 *
 * pg_dump_sort.c
 *	  Sort the items of a dump into a safe order for dumping
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_dump_sort.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"

/* translator: this is a module name */
static const char *modulename = gettext_noop("sorter");

/*
 * Sort priority for object types when dumping a pre-7.3 database.
 * Objects are sorted by priority levels, and within an equal priority level
 * by OID.  (This is a relatively crude hack to provide semi-reasonable
 * behavior for old databases without full dependency info.)  Note: collations,
 * extensions, text search, foreign-data, materialized view, event trigger,
 * policies, transforms, access methods and default ACL objects can't really
 * happen here, so the rather bogus priorities for them don't matter.
 *
 * NOTE: object-type priorities must match the section assignments made in
 * pg_dump.c; that is, PRE_DATA objects must sort before DO_PRE_DATA_BOUNDARY,
 * POST_DATA objects must sort after DO_POST_DATA_BOUNDARY, and DATA objects
 * must sort between them.
 */
static const int oldObjectTypePriority[] =
{
	1,							/* DO_NAMESPACE */
	1,							/* DO_EXTENSION */
	2,							/* DO_TYPE */
	2,							/* DO_SHELL_TYPE */
	2,							/* DO_FUNC */
	3,							/* DO_AGG */
	3,							/* DO_OPERATOR */
	3,							/* DO_ACCESS_METHOD */
	4,							/* DO_OPCLASS */
	4,							/* DO_OPFAMILY */
	4,							/* DO_COLLATION */
	5,							/* DO_CONVERSION */
	6,							/* DO_TABLE */
	8,							/* DO_ATTRDEF */
	15,							/* DO_INDEX */
	16,							/* DO_RULE */
	17,							/* DO_TRIGGER */
	14,							/* DO_CONSTRAINT */
	18,							/* DO_FK_CONSTRAINT */
	2,							/* DO_PROCLANG */
	2,							/* DO_CAST */
	11,							/* DO_TABLE_DATA */
	7,							/* DO_DUMMY_TYPE */
	4,							/* DO_TSPARSER */
	4,							/* DO_TSDICT */
	4,							/* DO_TSTEMPLATE */
	4,							/* DO_TSCONFIG */
	4,							/* DO_FDW */
	4,							/* DO_FOREIGN_SERVER */
	19,							/* DO_DEFAULT_ACL */
	4,							/* DO_TRANSFORM */
	9,							/* DO_BLOB */
	12,							/* DO_BLOB_DATA */
	10,							/* DO_PRE_DATA_BOUNDARY */
	13,							/* DO_POST_DATA_BOUNDARY */
	20,							/* DO_EVENT_TRIGGER */
	15,							/* DO_REFRESH_MATVIEW */
	21							/* DO_POLICY */
};

/*
 * Sort priority for object types when dumping newer databases.
 * Objects are sorted by type, and within a type by name.
 *
 * NOTE: object-type priorities must match the section assignments made in
 * pg_dump.c; that is, PRE_DATA objects must sort before DO_PRE_DATA_BOUNDARY,
 * POST_DATA objects must sort after DO_POST_DATA_BOUNDARY, and DATA objects
 * must sort between them.
 */
static const int newObjectTypePriority[] =
{
	1,							/* DO_NAMESPACE */
	4,							/* DO_EXTENSION */
	5,							/* DO_TYPE */
	5,							/* DO_SHELL_TYPE */
	6,							/* DO_FUNC */
	7,							/* DO_AGG */
	8,							/* DO_OPERATOR */
	8,							/* DO_ACCESS_METHOD */
	9,							/* DO_OPCLASS */
	9,							/* DO_OPFAMILY */
	3,							/* DO_COLLATION */
	11,							/* DO_CONVERSION */
	18,							/* DO_TABLE */
	20,							/* DO_ATTRDEF */
	27,							/* DO_INDEX */
	28,							/* DO_RULE */
	29,							/* DO_TRIGGER */
	26,							/* DO_CONSTRAINT */
	30,							/* DO_FK_CONSTRAINT */
	2,							/* DO_PROCLANG */
	10,							/* DO_CAST */
	23,							/* DO_TABLE_DATA */
	19,							/* DO_DUMMY_TYPE */
	12,							/* DO_TSPARSER */
	14,							/* DO_TSDICT */
	13,							/* DO_TSTEMPLATE */
	15,							/* DO_TSCONFIG */
	16,							/* DO_FDW */
	17,							/* DO_FOREIGN_SERVER */
	31,							/* DO_DEFAULT_ACL */
	3,							/* DO_TRANSFORM */
	21,							/* DO_BLOB */
	24,							/* DO_BLOB_DATA */
	22,							/* DO_PRE_DATA_BOUNDARY */
	25,							/* DO_POST_DATA_BOUNDARY */
	32,							/* DO_EVENT_TRIGGER */
	33,							/* DO_REFRESH_MATVIEW */
	34							/* DO_POLICY */
};

static DumpId preDataBoundId;
static DumpId postDataBoundId;


static int	DOTypeNameCompare(const void *p1, const void *p2);
static int	DOTypeOidCompare(const void *p1, const void *p2);
static bool TopoSort(DumpableObject **objs,
		 int numObjs,
		 DumpableObject **ordering,
		 int *nOrdering);
static void addHeapElement(int val, int *heap, int heapLength);
static int	removeHeapElement(int *heap, int heapLength);
static void findDependencyLoops(DumpableObject **objs, int nObjs, int totObjs);
static int findLoop(DumpableObject *obj,
		 DumpId startPoint,
		 bool *processed,
		 DumpId *searchFailed,
		 DumpableObject **workspace,
		 int depth);
static void repairDependencyLoop(DumpableObject **loop,
					 int nLoop);
static void describeDumpableObject(DumpableObject *obj,
					   char *buf, int bufsize);

static int	DOSizeCompare(const void *p1, const void *p2);

static int
findFirstEqualType(DumpableObjectType type, DumpableObject **objs, int numObjs)
{
	int			i;

	for (i = 0; i < numObjs; i++)
		if (objs[i]->objType == type)
			return i;
	return -1;
}

static int
findFirstDifferentType(DumpableObjectType type, DumpableObject **objs, int numObjs, int start)
{
	int			i;

	for (i = start; i < numObjs; i++)
		if (objs[i]->objType != type)
			return i;
	return numObjs - 1;
}

/*
 * When we do a parallel dump, we want to start with the largest items first.
 *
 * Say we have the objects in this order:
 * ....DDDDD....III....
 *
 * with D = Table data, I = Index, . = other object
 *
 * This sorting function now takes each of the D or I blocks and sorts them
 * according to their size.
 */
void
sortDataAndIndexObjectsBySize(DumpableObject **objs, int numObjs)
{
	int			startIdx,
				endIdx;
	void	   *startPtr;

	if (numObjs <= 1)
		return;

	startIdx = findFirstEqualType(DO_TABLE_DATA, objs, numObjs);
	if (startIdx >= 0)
	{
		endIdx = findFirstDifferentType(DO_TABLE_DATA, objs, numObjs, startIdx);
		startPtr = objs + startIdx;
		qsort(startPtr, endIdx - startIdx, sizeof(DumpableObject *),
			  DOSizeCompare);
	}

	startIdx = findFirstEqualType(DO_INDEX, objs, numObjs);
	if (startIdx >= 0)
	{
		endIdx = findFirstDifferentType(DO_INDEX, objs, numObjs, startIdx);
		startPtr = objs + startIdx;
		qsort(startPtr, endIdx - startIdx, sizeof(DumpableObject *),
			  DOSizeCompare);
	}
}

static int
DOSizeCompare(const void *p1, const void *p2)
{
	DumpableObject *obj1 = *(DumpableObject **) p1;
	DumpableObject *obj2 = *(DumpableObject **) p2;
	int			obj1_size = 0;
	int			obj2_size = 0;

	if (obj1->objType == DO_TABLE_DATA)
		obj1_size = ((TableDataInfo *) obj1)->tdtable->relpages;
	if (obj1->objType == DO_INDEX)
		obj1_size = ((IndxInfo *) obj1)->relpages;

	if (obj2->objType == DO_TABLE_DATA)
		obj2_size = ((TableDataInfo *) obj2)->tdtable->relpages;
	if (obj2->objType == DO_INDEX)
		obj2_size = ((IndxInfo *) obj2)->relpages;

	/* we want to see the biggest item go first */
	if (obj1_size > obj2_size)
		return -1;
	if (obj2_size > obj1_size)
		return 1;

	return 0;
}

/*
 * Sort the given objects into a type/name-based ordering
 *
 * Normally this is just the starting point for the dependency-based
 * ordering.
 */
void
sortDumpableObjectsByTypeName(DumpableObject **objs, int numObjs)
{
	if (numObjs > 1)
		qsort((void *) objs, numObjs, sizeof(DumpableObject *),
			  DOTypeNameCompare);
}

static int
DOTypeNameCompare(const void *p1, const void *p2)
{
	DumpableObject *obj1 = *(DumpableObject *const *) p1;
	DumpableObject *obj2 = *(DumpableObject *const *) p2;
	int			cmpval;

	/* Sort by type */
	cmpval = newObjectTypePriority[obj1->objType] -
		newObjectTypePriority[obj2->objType];

	if (cmpval != 0)
		return cmpval;

	/*
	 * Sort by namespace.  Note that all objects of the same type should
	 * either have or not have a namespace link, so we needn't be fancy about
	 * cases where one link is null and the other not.
	 */
	if (obj1->namespace && obj2->namespace)
	{
		cmpval = strcmp(obj1->namespace->dobj.name,
						obj2->namespace->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}

	/* Sort by name */
	cmpval = strcmp(obj1->name, obj2->name);
	if (cmpval != 0)
		return cmpval;

	/* To have a stable sort order, break ties for some object types */
	if (obj1->objType == DO_FUNC || obj1->objType == DO_AGG)
	{
		FuncInfo   *fobj1 = *(FuncInfo *const *) p1;
		FuncInfo   *fobj2 = *(FuncInfo *const *) p2;
		int			i;

		cmpval = fobj1->nargs - fobj2->nargs;
		if (cmpval != 0)
			return cmpval;
		for (i = 0; i < fobj1->nargs; i++)
		{
			TypeInfo   *argtype1 = findTypeByOid(fobj1->argtypes[i]);
			TypeInfo   *argtype2 = findTypeByOid(fobj2->argtypes[i]);

			if (argtype1 && argtype2)
			{
				if (argtype1->dobj.namespace && argtype2->dobj.namespace)
				{
					cmpval = strcmp(argtype1->dobj.namespace->dobj.name,
									argtype2->dobj.namespace->dobj.name);
					if (cmpval != 0)
						return cmpval;
				}
				cmpval = strcmp(argtype1->dobj.name, argtype2->dobj.name);
				if (cmpval != 0)
					return cmpval;
			}
		}
	}
	else if (obj1->objType == DO_OPERATOR)
	{
		OprInfo    *oobj1 = *(OprInfo *const *) p1;
		OprInfo    *oobj2 = *(OprInfo *const *) p2;

		/* oprkind is 'l', 'r', or 'b'; this sorts prefix, postfix, infix */
		cmpval = (oobj2->oprkind - oobj1->oprkind);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_ATTRDEF)
	{
		AttrDefInfo *adobj1 = *(AttrDefInfo *const *) p1;
		AttrDefInfo *adobj2 = *(AttrDefInfo *const *) p2;

		cmpval = (adobj1->adnum - adobj2->adnum);
		if (cmpval != 0)
			return cmpval;
	}

	/* Usually shouldn't get here, but if we do, sort by OID */
	return oidcmp(obj1->catId.oid, obj2->catId.oid);
}


/*
 * Sort the given objects into a type/OID-based ordering
 *
 * This is used with pre-7.3 source databases as a crude substitute for the
 * lack of dependency information.
 */
void
sortDumpableObjectsByTypeOid(DumpableObject **objs, int numObjs)
{
	if (numObjs > 1)
		qsort((void *) objs, numObjs, sizeof(DumpableObject *),
			  DOTypeOidCompare);
}

static int
DOTypeOidCompare(const void *p1, const void *p2)
{
	DumpableObject *obj1 = *(DumpableObject *const *) p1;
	DumpableObject *obj2 = *(DumpableObject *const *) p2;
	int			cmpval;

	cmpval = oldObjectTypePriority[obj1->objType] -
		oldObjectTypePriority[obj2->objType];

	if (cmpval != 0)
		return cmpval;

	return oidcmp(obj1->catId.oid, obj2->catId.oid);
}


/*
 * Sort the given objects into a safe dump order using dependency
 * information (to the extent we have it available).
 *
 * The DumpIds of the PRE_DATA_BOUNDARY and POST_DATA_BOUNDARY objects are
 * passed in separately, in case we need them during dependency loop repair.
 */
void
sortDumpableObjects(DumpableObject **objs, int numObjs,
					DumpId preBoundaryId, DumpId postBoundaryId)
{
	DumpableObject **ordering;
	int			nOrdering;

	if (numObjs <= 0)			/* can't happen anymore ... */
		return;

	/*
	 * Saving the boundary IDs in static variables is a bit grotty, but seems
	 * better than adding them to parameter lists of subsidiary functions.
	 */
	preDataBoundId = preBoundaryId;
	postDataBoundId = postBoundaryId;

	ordering = (DumpableObject **) pg_malloc(numObjs * sizeof(DumpableObject *));
	while (!TopoSort(objs, numObjs, ordering, &nOrdering))
		findDependencyLoops(ordering, nOrdering, numObjs);

	memcpy(objs, ordering, numObjs * sizeof(DumpableObject *));

	free(ordering);
}

/*
 * TopoSort -- topological sort of a dump list
 *
 * Generate a re-ordering of the dump list that satisfies all the dependency
 * constraints shown in the dump list.  (Each such constraint is a fact of a
 * partial ordering.)  Minimize rearrangement of the list not needed to
 * achieve the partial ordering.
 *
 * The input is the list of numObjs objects in objs[].  This list is not
 * modified.
 *
 * Returns TRUE if able to build an ordering that satisfies all the
 * constraints, FALSE if not (there are contradictory constraints).
 *
 * On success (TRUE result), ordering[] is filled with a sorted array of
 * DumpableObject pointers, of length equal to the input list length.
 *
 * On failure (FALSE result), ordering[] is filled with an unsorted array of
 * DumpableObject pointers of length *nOrdering, listing the objects that
 * prevented the sort from being completed.  In general, these objects either
 * participate directly in a dependency cycle, or are depended on by objects
 * that are in a cycle.  (The latter objects are not actually problematic,
 * but it takes further analysis to identify which are which.)
 *
 * The caller is responsible for allocating sufficient space at *ordering.
 */
static bool
TopoSort(DumpableObject **objs,
		 int numObjs,
		 DumpableObject **ordering,		/* output argument */
		 int *nOrdering)		/* output argument */
{
	DumpId		maxDumpId = getMaxDumpId();
	int		   *pendingHeap;
	int		   *beforeConstraints;
	int		   *idMap;
	DumpableObject *obj;
	int			heapLength;
	int			i,
				j,
				k;

	/*
	 * This is basically the same algorithm shown for topological sorting in
	 * Knuth's Volume 1.  However, we would like to minimize unnecessary
	 * rearrangement of the input ordering; that is, when we have a choice of
	 * which item to output next, we always want to take the one highest in
	 * the original list.  Therefore, instead of maintaining an unordered
	 * linked list of items-ready-to-output as Knuth does, we maintain a heap
	 * of their item numbers, which we can use as a priority queue.  This
	 * turns the algorithm from O(N) to O(N log N) because each insertion or
	 * removal of a heap item takes O(log N) time.  However, that's still
	 * plenty fast enough for this application.
	 */

	*nOrdering = numObjs;		/* for success return */

	/* Eliminate the null case */
	if (numObjs <= 0)
		return true;

	/* Create workspace for the above-described heap */
	pendingHeap = (int *) pg_malloc(numObjs * sizeof(int));

	/*
	 * Scan the constraints, and for each item in the input, generate a count
	 * of the number of constraints that say it must be before something else.
	 * The count for the item with dumpId j is stored in beforeConstraints[j].
	 * We also make a map showing the input-order index of the item with
	 * dumpId j.
	 */
	beforeConstraints = (int *) pg_malloc((maxDumpId + 1) * sizeof(int));
	memset(beforeConstraints, 0, (maxDumpId + 1) * sizeof(int));
	idMap = (int *) pg_malloc((maxDumpId + 1) * sizeof(int));
	for (i = 0; i < numObjs; i++)
	{
		obj = objs[i];
		j = obj->dumpId;
		if (j <= 0 || j > maxDumpId)
			exit_horribly(modulename, "invalid dumpId %d\n", j);
		idMap[j] = i;
		for (j = 0; j < obj->nDeps; j++)
		{
			k = obj->dependencies[j];
			if (k <= 0 || k > maxDumpId)
				exit_horribly(modulename, "invalid dependency %d\n", k);
			beforeConstraints[k]++;
		}
	}

	/*
	 * Now initialize the heap of items-ready-to-output by filling it with the
	 * indexes of items that already have beforeConstraints[id] == 0.
	 *
	 * The essential property of a heap is heap[(j-1)/2] >= heap[j] for each j
	 * in the range 1..heapLength-1 (note we are using 0-based subscripts
	 * here, while the discussion in Knuth assumes 1-based subscripts). So, if
	 * we simply enter the indexes into pendingHeap[] in decreasing order, we
	 * a-fortiori have the heap invariant satisfied at completion of this
	 * loop, and don't need to do any sift-up comparisons.
	 */
	heapLength = 0;
	for (i = numObjs; --i >= 0;)
	{
		if (beforeConstraints[objs[i]->dumpId] == 0)
			pendingHeap[heapLength++] = i;
	}

	/*--------------------
	 * Now emit objects, working backwards in the output list.  At each step,
	 * we use the priority heap to select the last item that has no remaining
	 * before-constraints.  We remove that item from the heap, output it to
	 * ordering[], and decrease the beforeConstraints count of each of the
	 * items it was constrained against.  Whenever an item's beforeConstraints
	 * count is thereby decreased to zero, we insert it into the priority heap
	 * to show that it is a candidate to output.  We are done when the heap
	 * becomes empty; if we have output every element then we succeeded,
	 * otherwise we failed.
	 * i = number of ordering[] entries left to output
	 * j = objs[] index of item we are outputting
	 * k = temp for scanning constraint list for item j
	 *--------------------
	 */
	i = numObjs;
	while (heapLength > 0)
	{
		/* Select object to output by removing largest heap member */
		j = removeHeapElement(pendingHeap, heapLength--);
		obj = objs[j];
		/* Output candidate to ordering[] */
		ordering[--i] = obj;
		/* Update beforeConstraints counts of its predecessors */
		for (k = 0; k < obj->nDeps; k++)
		{
			int			id = obj->dependencies[k];

			if ((--beforeConstraints[id]) == 0)
				addHeapElement(idMap[id], pendingHeap, heapLength++);
		}
	}

	/*
	 * If we failed, report the objects that couldn't be output; these are the
	 * ones with beforeConstraints[] still nonzero.
	 */
	if (i != 0)
	{
		k = 0;
		for (j = 1; j <= maxDumpId; j++)
		{
			if (beforeConstraints[j] != 0)
				ordering[k++] = objs[idMap[j]];
		}
		*nOrdering = k;
	}

	/* Done */
	free(pendingHeap);
	free(beforeConstraints);
	free(idMap);

	return (i == 0);
}

/*
 * Add an item to a heap (priority queue)
 *
 * heapLength is the current heap size; caller is responsible for increasing
 * its value after the call.  There must be sufficient storage at *heap.
 */
static void
addHeapElement(int val, int *heap, int heapLength)
{
	int			j;

	/*
	 * Sift-up the new entry, per Knuth 5.2.3 exercise 16. Note that Knuth is
	 * using 1-based array indexes, not 0-based.
	 */
	j = heapLength;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (val <= heap[i])
			break;
		heap[j] = heap[i];
		j = i;
	}
	heap[j] = val;
}

/*
 * Remove the largest item present in a heap (priority queue)
 *
 * heapLength is the current heap size; caller is responsible for decreasing
 * its value after the call.
 *
 * We remove and return heap[0], which is always the largest element of
 * the heap, and then "sift up" to maintain the heap invariant.
 */
static int
removeHeapElement(int *heap, int heapLength)
{
	int			result = heap[0];
	int			val;
	int			i;

	if (--heapLength <= 0)
		return result;
	val = heap[heapLength];		/* value that must be reinserted */
	i = 0;						/* i is where the "hole" is */
	for (;;)
	{
		int			j = 2 * i + 1;

		if (j >= heapLength)
			break;
		if (j + 1 < heapLength &&
			heap[j] < heap[j + 1])
			j++;
		if (val >= heap[j])
			break;
		heap[i] = heap[j];
		i = j;
	}
	heap[i] = val;
	return result;
}

/*
 * findDependencyLoops - identify loops in TopoSort's failure output,
 *		and pass each such loop to repairDependencyLoop() for action
 *
 * In general there may be many loops in the set of objects returned by
 * TopoSort; for speed we should try to repair as many loops as we can
 * before trying TopoSort again.  We can safely repair loops that are
 * disjoint (have no members in common); if we find overlapping loops
 * then we repair only the first one found, because the action taken to
 * repair the first might have repaired the other as well.  (If not,
 * we'll fix it on the next go-round.)
 *
 * objs[] lists the objects TopoSort couldn't sort
 * nObjs is the number of such objects
 * totObjs is the total number of objects in the universe
 */
static void
findDependencyLoops(DumpableObject **objs, int nObjs, int totObjs)
{
	/*
	 * We use three data structures here:
	 *
	 * processed[] is a bool array indexed by dump ID, marking the objects
	 * already processed during this invocation of findDependencyLoops().
	 *
	 * searchFailed[] is another array indexed by dump ID.  searchFailed[j] is
	 * set to dump ID k if we have proven that there is no dependency path
	 * leading from object j back to start point k.  This allows us to skip
	 * useless searching when there are multiple dependency paths from k to j,
	 * which is a common situation.  We could use a simple bool array for
	 * this, but then we'd need to re-zero it for each start point, resulting
	 * in O(N^2) zeroing work.  Using the start point's dump ID as the "true"
	 * value lets us skip clearing the array before we consider the next start
	 * point.
	 *
	 * workspace[] is an array of DumpableObject pointers, in which we try to
	 * build lists of objects constituting loops.  We make workspace[] large
	 * enough to hold all the objects in TopoSort's output, which is huge
	 * overkill in most cases but could theoretically be necessary if there is
	 * a single dependency chain linking all the objects.
	 */
	bool	   *processed;
	DumpId	   *searchFailed;
	DumpableObject **workspace;
	bool		fixedloop;
	int			i;

	processed = (bool *) pg_malloc0((getMaxDumpId() + 1) * sizeof(bool));
	searchFailed = (DumpId *) pg_malloc0((getMaxDumpId() + 1) * sizeof(DumpId));
	workspace = (DumpableObject **) pg_malloc(totObjs * sizeof(DumpableObject *));
	fixedloop = false;

	for (i = 0; i < nObjs; i++)
	{
		DumpableObject *obj = objs[i];
		int			looplen;
		int			j;

		looplen = findLoop(obj,
						   obj->dumpId,
						   processed,
						   searchFailed,
						   workspace,
						   0);

		if (looplen > 0)
		{
			/* Found a loop, repair it */
			repairDependencyLoop(workspace, looplen);
			fixedloop = true;
			/* Mark loop members as processed */
			for (j = 0; j < looplen; j++)
				processed[workspace[j]->dumpId] = true;
		}
		else
		{
			/*
			 * There's no loop starting at this object, but mark it processed
			 * anyway.  This is not necessary for correctness, but saves later
			 * invocations of findLoop() from uselessly chasing references to
			 * such an object.
			 */
			processed[obj->dumpId] = true;
		}
	}

	/* We'd better have fixed at least one loop */
	if (!fixedloop)
		exit_horribly(modulename, "could not identify dependency loop\n");

	free(workspace);
	free(searchFailed);
	free(processed);
}

/*
 * Recursively search for a circular dependency loop that doesn't include
 * any already-processed objects.
 *
 *	obj: object we are examining now
 *	startPoint: dumpId of starting object for the hoped-for circular loop
 *	processed[]: flag array marking already-processed objects
 *	searchFailed[]: flag array marking already-unsuccessfully-visited objects
 *	workspace[]: work array in which we are building list of loop members
 *	depth: number of valid entries in workspace[] at call
 *
 * On success, the length of the loop is returned, and workspace[] is filled
 * with pointers to the members of the loop.  On failure, we return 0.
 *
 * Note: it is possible that the given starting object is a member of more
 * than one cycle; if so, we will find an arbitrary one of the cycles.
 */
static int
findLoop(DumpableObject *obj,
		 DumpId startPoint,
		 bool *processed,
		 DumpId *searchFailed,
		 DumpableObject **workspace,
		 int depth)
{
	int			i;

	/*
	 * Reject if obj is already processed.  This test prevents us from finding
	 * loops that overlap previously-processed loops.
	 */
	if (processed[obj->dumpId])
		return 0;

	/*
	 * If we've already proven there is no path from this object back to the
	 * startPoint, forget it.
	 */
	if (searchFailed[obj->dumpId] == startPoint)
		return 0;

	/*
	 * Reject if obj is already present in workspace.  This test prevents us
	 * from going into infinite recursion if we are given a startPoint object
	 * that links to a cycle it's not a member of, and it guarantees that we
	 * can't overflow the allocated size of workspace[].
	 */
	for (i = 0; i < depth; i++)
	{
		if (workspace[i] == obj)
			return 0;
	}

	/*
	 * Okay, tentatively add obj to workspace
	 */
	workspace[depth++] = obj;

	/*
	 * See if we've found a loop back to the desired startPoint; if so, done
	 */
	for (i = 0; i < obj->nDeps; i++)
	{
		if (obj->dependencies[i] == startPoint)
			return depth;
	}

	/*
	 * Recurse down each outgoing branch
	 */
	for (i = 0; i < obj->nDeps; i++)
	{
		DumpableObject *nextobj = findObjectByDumpId(obj->dependencies[i]);
		int			newDepth;

		if (!nextobj)
			continue;			/* ignore dependencies on undumped objects */
		newDepth = findLoop(nextobj,
							startPoint,
							processed,
							searchFailed,
							workspace,
							depth);
		if (newDepth > 0)
			return newDepth;
	}

	/*
	 * Remember there is no path from here back to startPoint
	 */
	searchFailed[obj->dumpId] = startPoint;

	return 0;
}

/*
 * A user-defined datatype will have a dependency loop with each of its
 * I/O functions (since those have the datatype as input or output).
 * Similarly, a range type will have a loop with its canonicalize function,
 * if any.  Break the loop by making the function depend on the associated
 * shell type, instead.
 */
static void
repairTypeFuncLoop(DumpableObject *typeobj, DumpableObject *funcobj)
{
	TypeInfo   *typeInfo = (TypeInfo *) typeobj;

	/* remove function's dependency on type */
	removeObjectDependency(funcobj, typeobj->dumpId);

	/* add function's dependency on shell type, instead */
	if (typeInfo->shellType)
	{
		addObjectDependency(funcobj, typeInfo->shellType->dobj.dumpId);

		/*
		 * Mark shell type (always including the definition, as we need the
		 * shell type defined to identify the function fully) as to be dumped
		 * if any such function is
		 */
		if (funcobj->dump)
			typeInfo->shellType->dobj.dump = funcobj->dump |
				DUMP_COMPONENT_DEFINITION;
	}
}

/*
 * Because we force a view to depend on its ON SELECT rule, while there
 * will be an implicit dependency in the other direction, we need to break
 * the loop.  If there are no other objects in the loop then we can remove
 * the implicit dependency and leave the ON SELECT rule non-separate.
 * This applies to matviews, as well.
 */
static void
repairViewRuleLoop(DumpableObject *viewobj,
				   DumpableObject *ruleobj)
{
	/* remove rule's dependency on view */
	removeObjectDependency(ruleobj, viewobj->dumpId);
}

/*
 * However, if there are other objects in the loop, we must break the loop
 * by making the ON SELECT rule a separately-dumped object.
 *
 * Because findLoop() finds shorter cycles before longer ones, it's likely
 * that we will have previously fired repairViewRuleLoop() and removed the
 * rule's dependency on the view.  Put it back to ensure the rule won't be
 * emitted before the view.
 *
 * Note: this approach does *not* work for matviews, at the moment.
 */
static void
repairViewRuleMultiLoop(DumpableObject *viewobj,
						DumpableObject *ruleobj)
{
	TableInfo  *viewinfo = (TableInfo *) viewobj;
	RuleInfo   *ruleinfo = (RuleInfo *) ruleobj;
	int			i;

	/* remove view's dependency on rule */
	removeObjectDependency(viewobj, ruleobj->dumpId);
	/* pretend view is a plain table and dump it that way */
	viewinfo->relkind = 'r';	/* RELKIND_RELATION */
	/* mark rule as needing its own dump */
	ruleinfo->separate = true;
	/* move any reloptions from view to rule */
	if (viewinfo->reloptions)
	{
		ruleinfo->reloptions = viewinfo->reloptions;
		viewinfo->reloptions = NULL;
	}
	/* put back rule's dependency on view */
	addObjectDependency(ruleobj, viewobj->dumpId);
	/* now that rule is separate, it must be post-data */
	addObjectDependency(ruleobj, postDataBoundId);
	/* also, any triggers on the view must be dumped after the rule */
	for (i = 0; i < viewinfo->numTriggers; i++)
		addObjectDependency(&(viewinfo->triggers[i].dobj), ruleobj->dumpId);
}

/*
 * If a matview is involved in a multi-object loop, we can't currently fix
 * that by splitting off the rule.  As a stopgap, we try to fix it by
 * dropping the constraint that the matview be dumped in the pre-data section.
 * This is sufficient to handle cases where a matview depends on some unique
 * index, as can happen if it has a GROUP BY for example.
 *
 * Note that the "next object" is not necessarily the matview itself;
 * it could be the matview's rowtype, for example.  We may come through here
 * several times while removing all the pre-data linkages.
 */
static void
repairMatViewBoundaryMultiLoop(DumpableObject *matviewobj,
							   DumpableObject *boundaryobj,
							   DumpableObject *nextobj)
{
	TableInfo  *matviewinfo = (TableInfo *) matviewobj;

	/* remove boundary's dependency on object after it in loop */
	removeObjectDependency(boundaryobj, nextobj->dumpId);
	/* mark matview as postponed into post-data section */
	matviewinfo->postponed_def = true;
}

/*
 * Because we make tables depend on their CHECK constraints, while there
 * will be an automatic dependency in the other direction, we need to break
 * the loop.  If there are no other objects in the loop then we can remove
 * the automatic dependency and leave the CHECK constraint non-separate.
 */
static void
repairTableConstraintLoop(DumpableObject *tableobj,
						  DumpableObject *constraintobj)
{
	/* remove constraint's dependency on table */
	removeObjectDependency(constraintobj, tableobj->dumpId);
}

/*
 * However, if there are other objects in the loop, we must break the loop
 * by making the CHECK constraint a separately-dumped object.
 *
 * Because findLoop() finds shorter cycles before longer ones, it's likely
 * that we will have previously fired repairTableConstraintLoop() and
 * removed the constraint's dependency on the table.  Put it back to ensure
 * the constraint won't be emitted before the table...
 */
static void
repairTableConstraintMultiLoop(DumpableObject *tableobj,
							   DumpableObject *constraintobj)
{
	/* remove table's dependency on constraint */
	removeObjectDependency(tableobj, constraintobj->dumpId);
	/* mark constraint as needing its own dump */
	((ConstraintInfo *) constraintobj)->separate = true;
	/* put back constraint's dependency on table */
	addObjectDependency(constraintobj, tableobj->dumpId);
	/* now that constraint is separate, it must be post-data */
	addObjectDependency(constraintobj, postDataBoundId);
}

/*
 * Attribute defaults behave exactly the same as CHECK constraints...
 */
static void
repairTableAttrDefLoop(DumpableObject *tableobj,
					   DumpableObject *attrdefobj)
{
	/* remove attrdef's dependency on table */
	removeObjectDependency(attrdefobj, tableobj->dumpId);
}

static void
repairTableAttrDefMultiLoop(DumpableObject *tableobj,
							DumpableObject *attrdefobj)
{
	/* remove table's dependency on attrdef */
	removeObjectDependency(tableobj, attrdefobj->dumpId);
	/* mark attrdef as needing its own dump */
	((AttrDefInfo *) attrdefobj)->separate = true;
	/* put back attrdef's dependency on table */
	addObjectDependency(attrdefobj, tableobj->dumpId);
}

/*
 * CHECK constraints on domains work just like those on tables ...
 */
static void
repairDomainConstraintLoop(DumpableObject *domainobj,
						   DumpableObject *constraintobj)
{
	/* remove constraint's dependency on domain */
	removeObjectDependency(constraintobj, domainobj->dumpId);
}

static void
repairDomainConstraintMultiLoop(DumpableObject *domainobj,
								DumpableObject *constraintobj)
{
	/* remove domain's dependency on constraint */
	removeObjectDependency(domainobj, constraintobj->dumpId);
	/* mark constraint as needing its own dump */
	((ConstraintInfo *) constraintobj)->separate = true;
	/* put back constraint's dependency on domain */
	addObjectDependency(constraintobj, domainobj->dumpId);
	/* now that constraint is separate, it must be post-data */
	addObjectDependency(constraintobj, postDataBoundId);
}

/*
 * Fix a dependency loop, or die trying ...
 *
 * This routine is mainly concerned with reducing the multiple ways that
 * a loop might appear to common cases, which it passes off to the
 * "fixer" routines above.
 */
static void
repairDependencyLoop(DumpableObject **loop,
					 int nLoop)
{
	int			i,
				j;

	/* Datatype and one of its I/O or canonicalize functions */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TYPE &&
		loop[1]->objType == DO_FUNC)
	{
		repairTypeFuncLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TYPE &&
		loop[0]->objType == DO_FUNC)
	{
		repairTypeFuncLoop(loop[1], loop[0]);
		return;
	}

	/* View (including matview) and its ON SELECT rule */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_RULE &&
		(((TableInfo *) loop[0])->relkind == 'v' ||		/* RELKIND_VIEW */
		 ((TableInfo *) loop[0])->relkind == 'm') &&	/* RELKIND_MATVIEW */
		((RuleInfo *) loop[1])->ev_type == '1' &&
		((RuleInfo *) loop[1])->is_instead &&
		((RuleInfo *) loop[1])->ruletable == (TableInfo *) loop[0])
	{
		repairViewRuleLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_RULE &&
		(((TableInfo *) loop[1])->relkind == 'v' ||		/* RELKIND_VIEW */
		 ((TableInfo *) loop[1])->relkind == 'm') &&	/* RELKIND_MATVIEW */
		((RuleInfo *) loop[0])->ev_type == '1' &&
		((RuleInfo *) loop[0])->is_instead &&
		((RuleInfo *) loop[0])->ruletable == (TableInfo *) loop[1])
	{
		repairViewRuleLoop(loop[1], loop[0]);
		return;
	}

	/* Indirect loop involving view (but not matview) and ON SELECT rule */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE &&
				((TableInfo *) loop[i])->relkind == 'v')		/* RELKIND_VIEW */
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_RULE &&
						((RuleInfo *) loop[j])->ev_type == '1' &&
						((RuleInfo *) loop[j])->is_instead &&
						((RuleInfo *) loop[j])->ruletable == (TableInfo *) loop[i])
					{
						repairViewRuleMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* Indirect loop involving matview and data boundary */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE &&
				((TableInfo *) loop[i])->relkind == 'm')		/* RELKIND_MATVIEW */
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_PRE_DATA_BOUNDARY)
					{
						DumpableObject *nextobj;

						nextobj = (j < nLoop - 1) ? loop[j + 1] : loop[0];
						repairMatViewBoundaryMultiLoop(loop[i], loop[j],
													   nextobj);
						return;
					}
				}
			}
		}
	}

	/* Table and CHECK constraint */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[1])->contype == 'c' &&
		((ConstraintInfo *) loop[1])->contable == (TableInfo *) loop[0])
	{
		repairTableConstraintLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[0])->contype == 'c' &&
		((ConstraintInfo *) loop[0])->contable == (TableInfo *) loop[1])
	{
		repairTableConstraintLoop(loop[1], loop[0]);
		return;
	}

	/* Indirect loop involving table and CHECK constraint */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_CONSTRAINT &&
						((ConstraintInfo *) loop[j])->contype == 'c' &&
						((ConstraintInfo *) loop[j])->contable == (TableInfo *) loop[i])
					{
						repairTableConstraintMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* Table and attribute default */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_ATTRDEF &&
		((AttrDefInfo *) loop[1])->adtable == (TableInfo *) loop[0])
	{
		repairTableAttrDefLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_ATTRDEF &&
		((AttrDefInfo *) loop[0])->adtable == (TableInfo *) loop[1])
	{
		repairTableAttrDefLoop(loop[1], loop[0]);
		return;
	}

	/* Indirect loop involving table and attribute default */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_ATTRDEF &&
						((AttrDefInfo *) loop[j])->adtable == (TableInfo *) loop[i])
					{
						repairTableAttrDefMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* Domain and CHECK constraint */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TYPE &&
		loop[1]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[1])->contype == 'c' &&
		((ConstraintInfo *) loop[1])->condomain == (TypeInfo *) loop[0])
	{
		repairDomainConstraintLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TYPE &&
		loop[0]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[0])->contype == 'c' &&
		((ConstraintInfo *) loop[0])->condomain == (TypeInfo *) loop[1])
	{
		repairDomainConstraintLoop(loop[1], loop[0]);
		return;
	}

	/* Indirect loop involving domain and CHECK constraint */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TYPE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_CONSTRAINT &&
						((ConstraintInfo *) loop[j])->contype == 'c' &&
						((ConstraintInfo *) loop[j])->condomain == (TypeInfo *) loop[i])
					{
						repairDomainConstraintMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/*
	 * If all the objects are TABLE_DATA items, what we must have is a
	 * circular set of foreign key constraints (or a single self-referential
	 * table).  Print an appropriate complaint and break the loop arbitrarily.
	 */
	for (i = 0; i < nLoop; i++)
	{
		if (loop[i]->objType != DO_TABLE_DATA)
			break;
	}
	if (i >= nLoop)
	{
		write_msg(NULL, ngettext("NOTICE: there are circular foreign-key constraints on this table:\n",
								 "NOTICE: there are circular foreign-key constraints among these tables:\n",
								 nLoop));
		for (i = 0; i < nLoop; i++)
			write_msg(NULL, "  %s\n", loop[i]->name);
		write_msg(NULL, "You might not be able to restore the dump without using --disable-triggers or temporarily dropping the constraints.\n");
		write_msg(NULL, "Consider using a full dump instead of a --data-only dump to avoid this problem.\n");
		if (nLoop > 1)
			removeObjectDependency(loop[0], loop[1]->dumpId);
		else	/* must be a self-dependency */
			removeObjectDependency(loop[0], loop[0]->dumpId);
		return;
	}

	/*
	 * If we can't find a principled way to break the loop, complain and break
	 * it in an arbitrary fashion.
	 */
	write_msg(modulename, "WARNING: could not resolve dependency loop among these items:\n");
	for (i = 0; i < nLoop; i++)
	{
		char		buf[1024];

		describeDumpableObject(loop[i], buf, sizeof(buf));
		write_msg(modulename, "  %s\n", buf);
	}

	if (nLoop > 1)
		removeObjectDependency(loop[0], loop[1]->dumpId);
	else	/* must be a self-dependency */
		removeObjectDependency(loop[0], loop[0]->dumpId);
}

/*
 * Describe a dumpable object usefully for errors
 *
 * This should probably go somewhere else...
 */
static void
describeDumpableObject(DumpableObject *obj, char *buf, int bufsize)
{
	switch (obj->objType)
	{
		case DO_NAMESPACE:
			snprintf(buf, bufsize,
					 "SCHEMA %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_EXTENSION:
			snprintf(buf, bufsize,
					 "EXTENSION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TYPE:
			snprintf(buf, bufsize,
					 "TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_SHELL_TYPE:
			snprintf(buf, bufsize,
					 "SHELL TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FUNC:
			snprintf(buf, bufsize,
					 "FUNCTION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_AGG:
			snprintf(buf, bufsize,
					 "AGGREGATE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPERATOR:
			snprintf(buf, bufsize,
					 "OPERATOR %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_ACCESS_METHOD:
			snprintf(buf, bufsize,
					 "ACCESS METHOD %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPCLASS:
			snprintf(buf, bufsize,
					 "OPERATOR CLASS %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPFAMILY:
			snprintf(buf, bufsize,
					 "OPERATOR FAMILY %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_COLLATION:
			snprintf(buf, bufsize,
					 "COLLATION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CONVERSION:
			snprintf(buf, bufsize,
					 "CONVERSION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TABLE:
			snprintf(buf, bufsize,
					 "TABLE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_ATTRDEF:
			snprintf(buf, bufsize,
					 "ATTRDEF %s.%s  (ID %d OID %u)",
					 ((AttrDefInfo *) obj)->adtable->dobj.name,
					 ((AttrDefInfo *) obj)->adtable->attnames[((AttrDefInfo *) obj)->adnum - 1],
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_INDEX:
			snprintf(buf, bufsize,
					 "INDEX %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_REFRESH_MATVIEW:
			snprintf(buf, bufsize,
					 "REFRESH MATERIALIZED VIEW %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_RULE:
			snprintf(buf, bufsize,
					 "RULE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TRIGGER:
			snprintf(buf, bufsize,
					 "TRIGGER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_EVENT_TRIGGER:
			snprintf(buf, bufsize,
					 "EVENT TRIGGER %s (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CONSTRAINT:
			snprintf(buf, bufsize,
					 "CONSTRAINT %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FK_CONSTRAINT:
			snprintf(buf, bufsize,
					 "FK CONSTRAINT %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_PROCLANG:
			snprintf(buf, bufsize,
					 "PROCEDURAL LANGUAGE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CAST:
			snprintf(buf, bufsize,
					 "CAST %u to %u  (ID %d OID %u)",
					 ((CastInfo *) obj)->castsource,
					 ((CastInfo *) obj)->casttarget,
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_TRANSFORM:
			snprintf(buf, bufsize,
					 "TRANSFORM %u lang %u  (ID %d OID %u)",
					 ((TransformInfo *) obj)->trftype,
					 ((TransformInfo *) obj)->trflang,
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_TABLE_DATA:
			snprintf(buf, bufsize,
					 "TABLE DATA %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_DUMMY_TYPE:
			snprintf(buf, bufsize,
					 "DUMMY TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSPARSER:
			snprintf(buf, bufsize,
					 "TEXT SEARCH PARSER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSDICT:
			snprintf(buf, bufsize,
					 "TEXT SEARCH DICTIONARY %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSTEMPLATE:
			snprintf(buf, bufsize,
					 "TEXT SEARCH TEMPLATE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSCONFIG:
			snprintf(buf, bufsize,
					 "TEXT SEARCH CONFIGURATION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FDW:
			snprintf(buf, bufsize,
					 "FOREIGN DATA WRAPPER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FOREIGN_SERVER:
			snprintf(buf, bufsize,
					 "FOREIGN SERVER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_DEFAULT_ACL:
			snprintf(buf, bufsize,
					 "DEFAULT ACL %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_BLOB:
			snprintf(buf, bufsize,
					 "BLOB  (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_BLOB_DATA:
			snprintf(buf, bufsize,
					 "BLOB DATA  (ID %d)",
					 obj->dumpId);
			return;
		case DO_POLICY:
			snprintf(buf, bufsize,
					 "POLICY (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_PRE_DATA_BOUNDARY:
			snprintf(buf, bufsize,
					 "PRE-DATA BOUNDARY  (ID %d)",
					 obj->dumpId);
			return;
		case DO_POST_DATA_BOUNDARY:
			snprintf(buf, bufsize,
					 "POST-DATA BOUNDARY  (ID %d)",
					 obj->dumpId);
			return;
	}
	/* shouldn't get here */
	snprintf(buf, bufsize,
			 "object type %d  (ID %d OID %u)",
			 (int) obj->objType,
			 obj->dumpId, obj->catId.oid);
}
