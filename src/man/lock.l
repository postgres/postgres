.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/lock.l,v 1.9 1999/06/09 03:51:40 vadim Exp $
.TH LOCK SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
lock - Explicit lock of a table inside a transaction
.SH SYNOPSIS
.nf
\fBlock\fR [\fBtable\fR] classname
\fBlock\fR [\fBtable\fR] classname \fBin\fR [\fBrow\fR|\fBaccess\fR] {\fBshare\fR|\fBexclusive\fR} \fBmode\fR
\fBlock\fR [\fBtable\fR] classname \fBin\fR \fBshare row exclusive\fR \fBmode\fR
.fi
.SH DESCRIPTION
Available lock modes from least restrictive to most restrictive:
.PP
\fBACCESS SHARE MODE\fR

\fBNote\fR: this lock mode is acquired automatically over tables being
\queried. \fBPostgres\fR releases automatically acquired
ACCESS SHARE locks after statement is done.

This is the least restrictive lock mode which conflicts with ACCESS EXCLUSIVE
mode only. It's intended to protect table being queried from concurrent
\fBALTER TABLE\fR, \fBDROP TABLE\fR and
\fBVACUUM\fR statements over the same table.

\fBROW SHARE MODE\fR

\fBNote\fR: Automatically acquired by SELECT FOR UPDATE statement.

Conflicts with EXCLUSIVE and ACCESS EXCLUSIVE lock modes.

\fBROW EXCLUSIVE MODE\fR

\fBNote\fR: Automatically acquired by UPDATE, DELETE, INSERT statements.

Conflicts with SHARE, SHARE ROW EXCLUSIVE, EXCLUSIVE and ACCESS EXCLUSIVE
modes. Generally means that a transaction updated/inserted some tuples in a
table.

\fBSHARE MODE\fR

\fBNote\fR: Automatically acquired by CREATE INDEX statement.

Conflicts with ROW EXCLUSIVE, SHARE ROW EXCLUSIVE, EXCLUSIVE and ACCESS
EXCLUSIVE modes. This mode protects a table against concurrent updates.

\fBSHARE ROW EXCLUSIVE MODE\fR

Conflicts with ROW EXCLUSIVE, SHARE, SHARE ROW EXCLUSIVE, EXCLUSIVE and
ACCESS EXCLUSIVE modes. This mode is more restrictive than SHARE mode
because of only one transaction at time can hold this lock.

\fBEXCLUSIVE MODE\fR

Conflicts with ROW SHARE, ROW EXCLUSIVE, SHARE, SHARE ROW EXCLUSIVE,
EXCLUSIVE and ACCESS EXCLUSIVE modes. This mode is yet more restrictive than
SHARE ROW EXCLUSIVE one - it blocks concurrent SELECT FOR UPDATE queries.

\fBACCESS EXCLUSIVE MODE\fR

\fBNote\fR: Automatically acquired by ALTER TABLE, DROP TABLE, VACUUM
statements.

This is the most restrictive lock mode which conflicts with all other
lock modes and protects locked table from any concurrent operations.

\fBNote\fR: This lock mode is also acquired by first form of LOCK TABLE
(i.e. without explicit lock mode option).

.SH USAGE
.BR Postgres
always uses less restrictive lock modes ever possible. LOCK TABLE statement
provided for cases when you might need in more restrictive locking.
.PP
For example, application run transaction at READ COMMITTED isolation level
and need to ensure existance data in a table for duration of transaction. To
achieve this you could use SHARE lock mode over table before querying. This
will protect data from concurrent changes and provide your further read
operations over table with data in their real current state, because of
SHARE lock mode conflicts with ROW EXCLUSIVE one, acquired by writers, and
your LOCK TABLE table IN SHARE MODE statement will wait untill concurrent
write operations (if any) commit/rollback. (Note that to read data in their
real current state running transaction at SERIALIZABLE isolation level you
have to execute LOCK TABLE statement before execution any DML statement,
when transaction defines what concurrent changes will be visible to
herself).

If, in addition to requirements above, transaction is going to change data
in a table then SHARE ROW EXCLUSIVE lock mode should be acquired to prevent
deadlock conditions when two concurrent transactions would lock table in
SHARE mode and than would try to change data in this table, both
(implicitly) acquiring ROW EXCLUSIVE lock mode that conflicts with
concurrent SHARE lock.
  
Following deadlock issue (when two transaction wait one another)
touched above, you should follow two general rules to prevent 
deadlock conditions:
  
\fB1. Transactions have to acquire locks on the same objects in the same order.\fR
   
For example, if one application updates row R1 and than updates row R2 (in
the same transaction) then second application shouldn't update row R2 if
it's going update row R1 later (in single transaction).  Instead, it should
update R1 and R2 rows in the same order as first application.

\fB2. Transactions should acquire two conflicting lock modes only if one of
them is self-conflicting (i.e. may be held by one transaction at time only)
and should acquire most restrictive mode first.\fR
   
Example for this rule is described above when told about using
SHARE ROW EXCLUSIVE mode instead of SHARE one.

\fBNote\fR: \fBPostgres\fR does detect deadlocks and will rollback one of
waiting transactions to resolve the deadlock.

.SH COMPATIBILITY
LOCK TABLE statement is a \fBPostgres\fR language extension.

Except for ACCESS SHARE/EXCLUSIVE lock modes, all other \fBPostgres\fR lock
modes and LOCK TABLE statement syntax are compatible with \fBOracle\fR
ones.

.SH EXAMPLES
.nf
--
-- SHARE lock primary key table when going to perform
-- insert into foreign key table.
--
BEGIN WORK;
LOCK TABLE films IN SHARE MODE;
SELECT id FROM films 
  WHERE name = 'Star Wars: Episode I - The Phantom Menace';
--
-- Do ROLLBACK if record was not returned
--
INSERT INTO films_user_comments VALUES 
  (_id_, 'GREAT! I was waiting it so long!');
COMMIT WORK;

--
-- SHARE ROW EXCLUSIVE lock primary key table when going to perform
-- delete operation.
--
BEGIN WORK;
LOCK TABLE films IN SHARE ROW EXCLUSIVE MODE;
DELETE FROM films_user_comments WHERE id IN
  (SELECT id FROM films WHERE rating < 5);
DELETE FROM films WHERE rating < 5;
COMMIT WORK;

.SH "SEE ALSO"
begin(l),
commit(l),
set(l),
select(l).
