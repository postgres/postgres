.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_function.l,v 1.1 1996/11/14 10:16:53 scrappy Exp $
.TH "DROP FUNCTION" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
drop function \(em remove a user-defined C function 
.SH SYNOPSIS
.nf
\fBdrop function \fRfunction_name ( \fP[ type-1  { \fB,\fP type-n } ] \fB) 
.fi
.SH DESCRIPTION
.BR "drop function"
will remove references to an existing C function.  To execute this
command the user must be the owner of the function.  The input
argument types to the function must be specified, as only the
function with the given name and argument types will be removed.
.SH EXAMPLE
.nf
--
--this command removes the square root function
--
drop function sqrt(int4)
.fi
.SH "SEE ALSO"
create function(l).
.SH BUGS
No checks are made to ensure that types, operators or access methods
that rely on the function have been removed first.
