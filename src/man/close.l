.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/close.l,v 1.3 1998/01/11 22:17:09 momjian Exp $
.TH CLOSE SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
close - close a cursor
.SH SYNOPSIS
.nf
\fBclose\fP [cursor_name]
.fi
.SH DESCRIPTION
.BR Close
frees the resources associated with a cursor,
.IR cursor_name.
After this cursor is closed, no subsequent operations are allowed on
it.  A cursor should be closed when it is no longer needed.  If
.IR cursor_name. 
is not specified, then the blank cursor is closed.
.SH EXAMPLE
.nf
/*
 * close the cursor FOO
 */
close FOO
.fi
.SH "SEE ALSO"
fetch(l),
select(l).
