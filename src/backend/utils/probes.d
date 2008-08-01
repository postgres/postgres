/* ----------
 *	DTrace probes for PostgreSQL backend
 *
 *	Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/utils/probes.d,v 1.3 2008/08/01 13:16:09 alvherre Exp $
 * ----------
 */


/* typedefs used in PostgreSQL */
typedef unsigned int LocalTransactionId;
typedef int LWLockId;
typedef int LWLockMode;
typedef int LOCKMODE;
typedef unsigned int BlockNumber;
typedef unsigned int Oid;

#define bool char

provider postgresql {

	/* 
	 * Due to a bug in Mac OS X 10.5, using built-in typedefs (e.g. uintptr_t,
	 * uint32_t, etc.) cause compilation errors.  
	 */
	  
	probe transaction__start(LocalTransactionId);
	probe transaction__commit(LocalTransactionId);
	probe transaction__abort(LocalTransactionId);

	probe lwlock__acquire(LWLockId, LWLockMode);
	probe lwlock__release(LWLockId);
	probe lwlock__wait__start(LWLockId, LWLockMode);
	probe lwlock__wait__done(LWLockId, LWLockMode);
	probe lwlock__condacquire(LWLockId, LWLockMode);
	probe lwlock__condacquire__fail(LWLockId, LWLockMode);

	/* The following probe declarations cause compilation errors
         * on Mac OS X but not on Solaris. Need further investigation.
	 * probe lock__wait__start(unsigned int, LOCKMODE);
	 * probe lock__wait__done(unsigned int, LOCKMODE);
	 */
	probe lock__wait__start(unsigned int, int);
	probe lock__wait__done(unsigned int, int);

	probe query__parse__start(const char *);
	probe query__parse__done(const char *);
	probe query__rewrite__start(const char *);
	probe query__rewrite__done(const char *);
	probe query__plan__start();
	probe query__plan__done();
	probe query__execute__start();
	probe query__execute__done();
	probe query__start(const char *);
	probe query__done(const char *);
	probe statement__status(const char *);

	probe sort__start(int, bool, int, int, bool);
	probe sort__done(unsigned long, long);

	/* The following probe declarations cause compilation errors
         * on Mac OS X but not on Solaris. Need further investigation.
	 * probe buffer__read__start(BlockNumber, Oid, Oid, Oid, bool);
	 * probe buffer__read__done(BlockNumber, Oid, Oid, Oid, bool, bool);
	 */
	probe buffer__read__start(unsigned int, unsigned int, unsigned int, unsigned int, bool);
	probe buffer__read__done(unsigned int, unsigned int, unsigned int, unsigned int, bool, bool);

	probe buffer__flush__start(Oid, Oid, Oid);
	probe buffer__flush__done(Oid, Oid, Oid);

	probe buffer__hit(bool);
	probe buffer__miss(bool);
	probe buffer__checkpoint__start(int);
	probe buffer__checkpoint__done();
	probe buffer__sync__start(int, int);
	probe buffer__sync__written(int);
	probe buffer__sync__done(int, int, int);

	probe deadlock__found();

	probe clog__checkpoint__start(bool);
	probe clog__checkpoint__done(bool);
	probe subtrans__checkpoint__start(bool);
	probe subtrans__checkpoint__done(bool);
	probe multixact__checkpoint__start(bool);
	probe multixact__checkpoint__done(bool);
	probe twophase__checkpoint__start();
	probe twophase__checkpoint__done();
};
