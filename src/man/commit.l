.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/commit.l,v 1.4 1998/03/23 15:09:25 momjian Exp $
.TH COMMIT SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
commit - commit the current transaction 
.SH SYNOPSIS
.nf
\fBcommit [transaction|work]\fR
.fi
.SH DESCRIPTION
This commands commits the current transaction.  All changes made by
the transaction become visible to others and are guaranteed to be
durable if a crash occurs.
.IR "commit"
is functionally equivalent to the
.IR "end"
command
.SH "SEE ALSO"
abort(l),
begin(l),
commit(l),
rollback(l).
