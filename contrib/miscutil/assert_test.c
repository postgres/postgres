/*
 * assert_test.c --
 *
 * This file tests Postgres assert checking.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include "postgres.h"
#include "assert_test.h"

extern int	assertTest(int val);
extern int	assertEnable(int val);

int
assert_enable(int val)
{
	return assertEnable(val);
}

int
assert_test(int val)
{
	return assertTest(val);
}

/*

-- Enable/disable Postgres assert checking.
--
create function assert_enable(int4) returns int4
	as '/usr/local/pgsql/lib/assert_test.so'
	language 'C';

-- Test Postgres assert checking.
--
create function assert_test(int4) returns int4
	as '/usr/local/pgsql/lib/assert_test.so'
	language 'C';

*/

/* end of file */
