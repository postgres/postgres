.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_operator.l,v 1.3 1998/01/11 22:17:30 momjian Exp $
.TH "DROP OPERATOR" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop operator - remove an operator from the system
.SH SYNOPSIS
.nf
\fBdrop operator\fR opr_desc
.fi
.SH DESCRIPTION
This command drops an existing operator from the database.  To execute
this command you must be the owner of the operator.
.PP
.IR Opr_desc
is the name of the operator to be removed followed by a parenthesized
list of the operand types for the operator.  The left or right type
of a left or right unary operator, respectively, may be specified
as
.IR none .
.PP
It is the user's responsibility to remove any access methods, operator
classes, etc. that rely on the deleted operator.
.SH EXAMPLE
.nf
--
--Remove power operator a^n for int4
--
drop operator ^ (int4, int4)
.fi
.nf
--
--Remove left unary operator !a for booleans
-- 
drop operator ! (none, bool)
.fi
.nf
--
--Remove right unary factorial operator a! for int4
--
drop operator ! (int4, none)
.fi
.SH "SEE ALSO"
create operator(l).
