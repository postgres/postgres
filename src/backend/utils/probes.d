/* ----------
 *	DTrace probes for PostgreSQL backend
 *
 *	Copyright (c) 2006-2016, PostgreSQL Global Development Group
 *
 *	src/backend/utils/probes.d
 * ----------
 */


/*
 * Typedefs used in PostgreSQL.
 *
 * NOTE: Do not use system-provided typedefs (e.g. uintptr_t, uint32_t, etc)
 * in probe definitions, as they cause compilation errors on Mac OS X 10.5.
 */
#define LocalTransactionId unsigned int
#define LWLockMode int
#define LOCKMODE int
#define BlockNumber unsigned int
#define Oid unsigned int
#define ForkNumber int
#define bool char

provider postgresql {

	probe transaction__start(LocalTransactionId);
	probe transaction__commit(LocalTransactionId);
	probe transaction__abort(LocalTransactionId);

	probe lwlock__acquire(const char *, int, LWLockMode);
	probe lwlock__release(const char *, int);
	probe lwlock__wait__start(const char *, int, LWLockMode);
	probe lwlock__wait__done(const char *, int, LWLockMode);
	probe lwlock__condacquire(const char *, int, LWLockMode);
	probe lwlock__condacquire__fail(const char *, int, LWLockMode);
	probe lwlock__acquire__or__wait(const char *, int, LWLockMode);
	probe lwlock__acquire__or__wait__fail(const char *, int, LWLockMode);

	probe lock__wait__start(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, LOCKMODE);
	probe lock__wait__done(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, LOCKMODE);

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
	probe sort__done(bool, long);

	probe buffer__read__start(ForkNumber, BlockNumber, Oid, Oid, Oid, int, bool);
	probe buffer__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, bool, bool);
	probe buffer__flush__start(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe buffer__flush__done(ForkNumber, BlockNumber, Oid, Oid, Oid);

	probe buffer__checkpoint__start(int);
	probe buffer__checkpoint__sync__start();
	probe buffer__checkpoint__done();
	probe buffer__sync__start(int, int);
	probe buffer__sync__written(int);
	probe buffer__sync__done(int, int, int);
	probe buffer__write__dirty__start(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe buffer__write__dirty__done(ForkNumber, BlockNumber, Oid, Oid, Oid);

	probe deadlock__found();

	probe checkpoint__start(int);
	probe checkpoint__done(int, int, int, int, int);
	probe clog__checkpoint__start(bool);
	probe clog__checkpoint__done(bool);
	probe subtrans__checkpoint__start(bool);
	probe subtrans__checkpoint__done(bool);
	probe multixact__checkpoint__start(bool);
	probe multixact__checkpoint__done(bool);
	probe twophase__checkpoint__start();
	probe twophase__checkpoint__done();

	probe smgr__md__read__start(ForkNumber, BlockNumber, Oid, Oid, Oid, int);
	probe smgr__md__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, int, int);
	probe smgr__md__write__start(ForkNumber, BlockNumber, Oid, Oid, Oid, int);
	probe smgr__md__write__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, int, int);

	probe xlog__insert(unsigned char, unsigned char);
	probe xlog__switch();
	probe wal__buffer__write__dirty__start();
	probe wal__buffer__write__dirty__done();
};
