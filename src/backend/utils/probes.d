/* ----------
 *	DTrace probes for PostgreSQL backend
 *
 *	Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/utils/probes.d,v 1.2 2008/01/02 02:42:06 momjian Exp $
 * ----------
 */

provider postgresql {

probe transaction__start(int);
probe transaction__commit(int);
probe transaction__abort(int);
probe lwlock__acquire(int, int);
probe lwlock__release(int);
probe lwlock__startwait(int, int);
probe lwlock__endwait(int, int);
probe lwlock__condacquire(int, int);
probe lwlock__condacquire__fail(int, int);
probe lock__startwait(int, int);
probe lock__endwait(int, int);

};
