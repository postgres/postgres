.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/end.l,v 1.1.2.1 1996/12/11 00:31:33 momjian Exp $
.TH END SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
end \(em commit the current transaction 
.SH SYNOPSIS
.nf
\fBend [transaction]\fR
.fi
.SH DESCRIPTION
This commands commits the current transaction.  All changes made by
the transaction become visible to others and are guaranteed to be
durable if a crash occurs.
.SH "SEE ALSO"
abort(l),
begin(l).
