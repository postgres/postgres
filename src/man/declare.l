.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/declare.l,v 1.5 1998/04/28 18:39:11 momjian Exp $
.TH FETCH SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
declare - declare a cursor
.SH SYNOPSIS
.nf
\fBdeclare\fR cursor_name [ \fBbinary\fR ] \fBcursor for\fR select statement
.fi
.SH DESCRIPTION
.BR Declare
allows a user to create cursors.
Cursors are only available in transactions.
.PP
Normal cursors return data back in ASCII format.  Since data is stored
natively in binary format, the system must do a conversion to produce
the ASCII format.  In addition, ASCII formats are often larger in size
than binary format.  Once the attributes come back in ASCII, often the
client application then has to convert it to a binary format to
manipulate it anyway.
.PP
\fBBinary\fR cursors give you back the data in the native binary
representation.  Thus, binary cursors will tend to be a little faster
since there's less overhead of conversion.
.PP
However, ASCII is architectural neutral whereas binary representation
can differ between different machine architecture.  Thus, if your client
machine uses a different representation than you server machine, getting
back attributes in binary format is probably not what you want. Also, if
your main purpose is displaying the data in ASCII, then getting it back
in ASCII will save you some effort on the client side.
.PP
For an example, see the fetch(l) manual page.
.SH "SEE ALSO"
fetch(l),
begin(l),
commit(l),
select(l).
