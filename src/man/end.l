.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/end.l,v 1.4 1998/03/25 01:54:52 momjian Exp $
.TH END SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
end - commit the current transaction 
.SH SYNOPSIS
.nf
\fBend [transaction]\fR
.fi
.SH DESCRIPTION
This commands commits the current transaction.  All changes made by
the transaction become visible to others and are guaranteed to be
durable if a crash occurs.
.SH "SEE ALSO"
rollback(l),
begin(l).
