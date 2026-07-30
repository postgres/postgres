/* ----------
 *	DTrace probes for PostgreSQL backend
 *
 *	Copyright (c) 2006-2026, PostgreSQL Global Development Group
 *
 *	src/backend/utils/probes.d
 * ----------
 */


/*
 * Typedefs used in PostgreSQL probes.
 *
 * NOTE: Prefer system-supplied type names in probe definitions (e.g.
 * uint64_t, not PostgreSQL's uint64); macOS' dtrace rejects the latter.
 * Also avoid "long long int", as SystemTap's dtrace(1) fails on that
 * specific spelling.
 *
 * But we can use some PG type aliases by #define'ing them here so that
 * cpp expands them before dtrace sees the file.  Curiously, macOS' dtrace
 * rejects "bool", so we handle that with a #define too.
 */
#define LocalTransactionId unsigned int
#define LWLockMode int
#define LOCKMODE int
#define BlockNumber unsigned int
#define Oid unsigned int
#define ForkNumber int
#define bool unsigned char

provider postgresql {

	probe transaction__start(LocalTransactionId);
	probe transaction__commit(LocalTransactionId);
	probe transaction__abort(LocalTransactionId);

	probe lwlock__acquire(const char *, LWLockMode);
	probe lwlock__release(const char *);
	probe lwlock__wait__start(const char *, LWLockMode);
	probe lwlock__wait__done(const char *, LWLockMode);
	probe lwlock__condacquire(const char *, LWLockMode);
	probe lwlock__condacquire__fail(const char *, LWLockMode);
	probe lwlock__acquire__or__wait(const char *, LWLockMode);
	probe lwlock__acquire__or__wait__fail(const char *, LWLockMode);

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

	probe sort__start(int, bool, int, int, bool, int);
	probe sort__done(bool, long);

	probe buffer__read__start(ForkNumber, BlockNumber, Oid, Oid, Oid, int);
	probe buffer__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, bool);
	probe buffer__flush__start(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe buffer__flush__done(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe buffer__extend__start(ForkNumber, Oid, Oid, Oid, int, unsigned int);
	probe buffer__extend__done(ForkNumber, Oid, Oid, Oid, int, unsigned int, BlockNumber);

	probe buffer__checkpoint__start(int);
	probe buffer__checkpoint__sync__start();
	probe buffer__checkpoint__done();
	probe buffer__sync__start(int, int);
	probe buffer__sync__written(int);
	probe buffer__sync__done(int, int, int);

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
	probe smgr__md__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, ssize_t, size_t);
	probe smgr__md__write__start(ForkNumber, BlockNumber, Oid, Oid, Oid, int);
	probe smgr__md__write__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, ssize_t, size_t);

	probe wal__insert(unsigned char, unsigned char);
	probe wal__switch();
	probe wal__buffer__write__dirty__start();
	probe wal__buffer__write__dirty__done();
};
