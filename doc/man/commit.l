.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/commit.l,v 1.1.1.1 1996/08/18 22:14:20 scrappy Exp $
.TH COMMIT SQL 01/23/93 Postgres95 Postgres95
.SH NAME
commit \(em commit the current transaction 
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
end(l),
rollback(l).
