/*
 * misc_utils.c --
 *
 * This file defines miscellaneous PostgreSQL utility functions.
 *
 * Copyright (c) 1998, Massimo Dal Zotto <dz@cs.unitn.it>
 *
 * This file is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */

#include <unistd.h>

#include "postgres.h"
#include "utils/palloc.h"

#include "misc_utils.h"
#include "assert_test.h"

extern int	ExecutorLimit(int limit);
extern void Async_Unlisten(char *relname, int pid);
extern int	assertTest(int val);

#ifdef ASSERT_CHECKING_TEST
extern int	assertEnable(int val);

#endif

int
query_limit(int limit)
{
	return ExecutorLimit(limit);
}

int
backend_pid()
{
	return getpid();
}

int
unlisten(char *relname)
{
	Async_Unlisten(relname, getpid());
	return 0;
}

int
max(int x, int y)
{
	return ((x > y) ? x : y);
}

int
min(int x, int y)
{
	return ((x < y) ? x : y);
}

int
assert_enable(int val)
{
	return assertEnable(val);
}

#ifdef ASSERT_CHECKING_TEST
int
assert_test(int val)
{
	return assertTest(val);
}

#endif

/* end of file */

/*
 * Local variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
