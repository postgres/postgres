.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/begin.l,v 1.6 1999/06/11 05:40:18 vadim Exp $
.TH BEGIN SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
begin - begins a transaction in chained mode
.SH SYNOPSIS
.nf
\fBbegin\fP \fB[transaction|work]\fR
.fi
.SH DESCRIPTION

By default, Postgres executes transactions in unchained mode (also known as
autocommit feature in other DBMSes). In other words, each user statement is
executed in its own transaction and commit is implicit (if execution was
successfull). BEGIN initiates a user transaction in chained mode, i.e. all
user statements after BEGIN command will be executed in single transaction
untill explicit COMMIT, ROLLBACK or execution abort. Statements in chained
mode are executed much faster, because of transaction start/commit requires
significant CPU and disk activity. This mode is also required for
consistency when changing one of related tables.

Default transaction isolation level in Postgres is READ COMMITTED one, when
queries inside transaction see only changes committed before query
execution. So, you have to use SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
command just after BEGIN if you need in better transaction isolation.  In
SERIALIZABLE mode queries will see only changes committed before entire
transaction began (actually, before execution of first DML statement in
serializable transaction).

If the transaction is committed, Postgres will ensure either that all
updates are done or else that none of them are done. Transactions have the
standard ACID (atomic, consistent, isolatable, and durable) property.

\fBNote\fR: There is no explicit BEGIN command in SQL92; transaction
initiation is always implicit and it terminates either with a COMMIT or with
a ROLLBACK statement (i.e. all transactions are chained). SQL92 also
requires SERIALIZABLE to be default transaction isolation level.

.SH "SEE ALSO"
rollback(l),
commit(l),
set(l),
lock(l).
