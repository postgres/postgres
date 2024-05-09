/*
 * Mksort (multiple-key sort) is an alternative of standard qsort algorithm,
 * which has better performance for particular sort scenarios, i.e. the
 * data set has multiple keys to be sorted.
 *
 * The sorting algorithm blends Quicksort and radix sort; Like regular
 * Quicksort, it partitions its input into sets less than and greater than a
 * given value; like radix sort, it moves on to the next field once the current
 * input is known to be equal in the given field.
 *
 * The implementation is based on the paper:
 *   Jon L. Bentley and Robert Sedgewick, "Fast Algorithms for Sorting and
 *   Searching Strings", Jan 1997
 *
 * Some improvements which is related to additional handling for equal tuples
 * have been adapted to keep consistency with the implementations of postgres
 * qsort.
 *
 * For now, mksort_tuple() is called in tuplesort_sort_memtuples() as a
 * replacement of qsort_tuple() when specific conditions are satisfied.
 */

/* Swap two tuples in sort tuple array */
static inline void
mksort_swap(int a,
			int b,
			SortTuple *x)
{
	SortTuple t;

	if (a == b)
		return;
	t = x[a];
	x[a] = x[b];
	x[b] = t;
}

/* Swap tuples by batch in sort tuple array */
static inline void
mksort_vec_swap(int a,
				int b,
				int size,
				SortTuple *x)
{
	while (size-- > 0)
	{
		mksort_swap(a, b, x);
		a++;
		b++;
	}
}

/*
 * Check whether current datum (at specified tuple and depth) is null
 * Note that the input x means a specified tuple provided by caller but not
 * a tuple array, so tupleIndex is unnecessary
 */
static inline bool
check_datum_null(SortTuple *x,
				 int depth,
				 Tuplesortstate *state)
{
	Datum datum;
	bool isNull;

	/* Since we have a specified tuple, the tupleIndex is always 0 */
	state->base.mksortGetDatumFunc(x, 0, depth, state, &datum, &isNull, false);

	/*
	 * Note: for "abbreviated key", we don't need to handle more here because
	 * if "abbreviated key" of a datum is null, the "full" datum must be null.
	 */

	return isNull;
}

/*
 * Compare two tuples at specified depth
 *
 * If "abbreviated key" is disabled:
 *   get specified datums and compare them by ApplySortComparator().
 * If "abbreviated key" is enabled:
 *   Only first datum may be abbr key according to the design (see the comments
 *   of struct SortTuple), so different operations are needed for different
 *   datum.
 *   For first datum (depth == 0): get first datums ("abbr key" version) and
 *   compare them by ApplySortComparator(). If they are equal, get "full"
 *   version and compare again by ApplySortAbbrevFullComparator().
 *   For other datums: get specified datums and compare them by
 *   ApplySortComparator() as regular routine does.
 *
 * See comparetup_heap() for details.
 */
static inline int
mksort_compare_datum(SortTuple      *tuple1,
					 SortTuple      *tuple2,
					 int			 depth,
					 Tuplesortstate *state)
{
	Datum datum1, datum2;
	bool isNull1, isNull2;
	SortSupport sortKey;
	int ret = 0;

	Assert(state->mksortGetDatumFunc);

	sortKey = state->base.sortKeys + depth;
	state->base.mksortGetDatumFunc(tuple1, 0, depth, state,
							  &datum1, &isNull1, false);
	state->base.mksortGetDatumFunc(tuple2, 0, depth, state,
							  &datum2, &isNull2, false);

	ret = ApplySortComparator(datum1,
							  isNull1,
							  datum2,
							  isNull2,
							  sortKey);

	/*
	 * If "abbreviated key" is enabled, and we are in the first depth, it means
	 * only "abbreviated keys" are compared. If the two datums are determined to
	 * be equal by ApplySortComparator(), we need to perform an extra "full"
	 * comparing by ApplySortAbbrevFullComparator().
	 */
	if (sortKey->abbrev_converter &&
		depth == 0 &&
		ret == 0)
	{
		/* Fetch "full" datum by setting useFullKey = true */
		state->base.mksortGetDatumFunc(tuple1, 0, depth, state,
								  &datum1, &isNull1, true);
		state->base.mksortGetDatumFunc(tuple2, 0, depth, state,
								  &datum2, &isNull2, true);

		ret = ApplySortAbbrevFullComparator(datum1,
											isNull1,
											datum2,
											isNull2,
											sortKey);
	}

	return ret;
}

#ifdef MKSORT_VERIFY
/*
 * Verify whether the SortTuple list is ordered or not at specified depth
 */
static void
mksort_verify(SortTuple		  *x,
			  int			   n,
			  int			   depth,
			  Tuplesortstate  *state)
{
	int ret;

	for (int i = 0;i < n - 1;i++)
	{
		ret = mksort_compare_datum(x + i,
								   x + i + 1,
								   depth,
								   state);
		Assert(ret <= 0);
	}
}
#endif

/*
 * Major of multi-key sort
 *
 * seenNull indicates whether we have seen NULL in any datum we checked
 */
static void
mksort_tuple(SortTuple           *x,
			 size_t               n,
			 int                  depth,
			 Tuplesortstate      *state,
			 bool				  seenNull)
{
	/*
	 * In the process, the tuple array consists of five parts:
	 * left equal, less, not-processed, greater, right equal
	 *
	 * lessStart indicates the first position of less part
	 * lessEnd indicates the next position after less part
	 * greaterStart indicates the prior position before greater part
	 * greaterEnd indicates the latest position of greater part
	 * the range between lessEnd and greaterStart (inclusive) is not-processed
	 */
	int lessStart, lessEnd, greaterStart, greaterEnd, tupCount;
	int32 dist;
	SortTuple *pivot;
	bool isDatumNull;

	Assert(depth <= state->base.nKeys);
	Assert(state->base.sortKeys);
	Assert(state->base.mksortGetDatumFunc);

	if (n <= 1)
		return;

	/* If we have exceeded the max depth, return immediately */
	if (depth == state->base.nKeys)
		return;

	CHECK_FOR_INTERRUPTS();

	/* Select pivot by random and move it to the first position */
	lessStart = pg_prng_int64p(&pg_global_prng_state) % n;
	mksort_swap(0, lessStart, x);
	pivot = x;

	lessStart = 1;
	lessEnd = 1;
	greaterStart = n - 1;
	greaterEnd = n - 1;

	/* Sort the array to three parts: lesser, equal, greater */
	while (true)
	{
		CHECK_FOR_INTERRUPTS();

		/* Compare the left end of the array */
		while (lessEnd <= greaterStart)
		{
			/* Compare lessEnd and pivot at current depth */
			dist = mksort_compare_datum(x + lessEnd,
										pivot,
										depth,
										state);

			if (dist > 0)
				break;

			/* If lessEnd is equal to pivot, move it to lessStart */
			if (dist == 0)
			{
				mksort_swap(lessEnd, lessStart, x);
				lessStart++;
			}
			lessEnd++;
		}

		/* Compare the right end of the array */
		while (lessEnd <= greaterStart)
		{
			/* Compare greaterStart and pivot at current depth */
			dist = mksort_compare_datum(x + greaterStart,
										pivot,
										depth,
										state);

			if (dist < 0)
				break;

			/* If greaterStart is equal to pivot, move it to greaterEnd */
			if (dist == 0)
			{
				mksort_swap(greaterStart, greaterEnd, x);
				greaterEnd--;
			}
			greaterStart--;
		}

		if (lessEnd > greaterStart)
			break;
		mksort_swap(lessEnd, greaterStart, x);
		lessEnd++;
		greaterStart--;
	}

	/*
	 * Now the array has four parts:
	 *   left equal, lesser, greater, right equal
	 * Note greaterStart is less than lessEnd now
	 */

	/* Move the left equal part to middle */
	dist = Min(lessStart, lessEnd - lessStart);
	mksort_vec_swap(0, lessEnd - dist, dist, x);

	/* Move the right equal part to middle */
	dist = Min(greaterEnd - greaterStart, n - greaterEnd - 1);
	mksort_vec_swap(lessEnd, n - dist, dist, x);

	/*
	 * Now the array has three parts:
	 *   lesser, equal, greater
	 * Note that one or two parts may have no element at all.
	 */

	/* Recursively sort the lesser part */

	/* dist means the size of less part */
	dist = lessEnd - lessStart;
	mksort_tuple(x,
				 dist,
				 depth,
				 state,
				 seenNull);

	/* Recursively sort the equal part */

	/*
	 * (x + dist) means the first tuple in the equal part
	 * Since all tuples have equal datums at current depth, we just check any one
	 * of them to determine whether we have seen null datum.
	 */
	isDatumNull = check_datum_null(x + dist, depth, state);

	/* (lessStart + n - greaterEnd - 1) means the size of equal part */
	tupCount = lessStart + n - greaterEnd - 1;

	if (depth < state->base.nKeys - 1)
	{
		mksort_tuple(x + dist,
					 tupCount,
					 depth + 1,
					 state,
					 seenNull || isDatumNull);
	} else {
		/*
		 * We have reach the max depth: Call mksortHandleDupFunc to handle duplicated
		 * tuples if necessary, e.g. checking uniqueness or extra comparing
		 */

		/*
		 * Call mksortHandleDupFunc if:
		 *   1. mksortHandleDupFunc is filled
		 *   2. the size of equal part > 1
		 */
		if (state->base.mksortHandleDupFunc &&
			(tupCount > 1))
		{
			state->base.mksortHandleDupFunc(x + dist,
									   tupCount,
									   seenNull || isDatumNull,
									   state);
		}
	}

	/* Recursively sort the greater part */

	/* dist means the size of greater part */
	dist = greaterEnd - greaterStart;
	mksort_tuple(x + n - dist,
				 dist,
				 depth,
				 state,
				 seenNull);

#ifdef MKSORT_VERIFY
	mksort_verify(x,
				  n,
				  depth,
				  state);
#endif
}
