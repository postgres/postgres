/* ----------
 *	DTrace probes for PostgreSQL backend
 *
 *	Copyright (c) 2006-2009, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/utils/probes.d,v 1.10 2009/04/02 19:14:34 momjian Exp $
 * ----------
 */


/*
 * Typedefs used in PostgreSQL.
 *
 * NOTE: Do not use system-provided typedefs (e.g. uintptr_t, uint32_t, etc)
 * in probe definitions, as they cause compilation errors on Mac OS X 10.5.
 */
#define LocalTransactionId unsigned int
#define TransactionId unsigned int
#define LWLockId int
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

	probe lwlock__acquire(LWLockId, LWLockMode);
	probe lwlock__release(LWLockId);
	probe lwlock__wait__start(LWLockId, LWLockMode);
	probe lwlock__wait__done(LWLockId, LWLockMode);
	probe lwlock__condacquire(LWLockId, LWLockMode);
	probe lwlock__condacquire__fail(LWLockId, LWLockMode);

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

	probe buffer__read__start(ForkNumber, BlockNumber, Oid, Oid, Oid, bool, bool);
	probe buffer__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, bool, bool, bool);
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

	probe smgr__md__read__start(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe smgr__md__read__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, int);
	probe smgr__md__write__start(ForkNumber, BlockNumber, Oid, Oid, Oid);
	probe smgr__md__write__done(ForkNumber, BlockNumber, Oid, Oid, Oid, int, int);

	probe xlog__insert(unsigned char, unsigned char);
	probe xlog__switch();
	probe wal__buffer__write__dirty__start();
	probe wal__buffer__write__dirty__done();

	probe slru__readpage__start(unsigned long, int, bool, TransactionId);
	probe slru__readpage__done(int);
	probe slru__readpage__readonly(unsigned long, int, TransactionId);
	probe slru__writepage__start(unsigned long, int, int);
	probe slru__writepage__done();
	probe slru__readpage__physical__start(unsigned long, char *, int, int);
	probe slru__readpage__physical__done(int, int, int);
	probe slru__writepage__physical__start(unsigned long, int, int);
	probe slru__writepage__physical__done(int, int, int);
 
	probe executor__scan(unsigned long, unsigned int, unsigned long);
	probe executor__agg(unsigned long, int);
	probe executor__group(unsigned long, int);
	probe executor__hash__multi(unsigned long);
	probe executor__hashjoin(unsigned long);
	probe executor__limit(unsigned long);
	probe executor__material(unsigned long);
	probe executor__mergejoin(unsigned long);
	probe executor__nestloop(unsigned long);
	probe executor__setop(unsigned long);
	probe executor__sort(unsigned long, int);
	probe executor__subplan__hash(unsigned long);
	probe executor__subplan__scan(unsigned long);
	probe executor__unique(unsigned long);
};
