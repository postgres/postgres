.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/delete.l,v 1.1.1.1 1996/08/18 22:14:23 scrappy Exp $
.TH DELETE SQL 11/05/95 Postgres95 Postgres95
.SH NAME
delete \(em delete instances from a class
.SH SYNOPSIS
.nf
\fBdelete\fR instance_variable [ \fBfrom\fR from_list ] [ \fBwhere\fR qual ]
.fi
.SH DESCRIPTION
.BR Delete
removes instances which satisfy the qualification,
.IR qual ,
from the class specified by
.IR instance_variable .
.IR Instance_variable
is either a class name or a variable assigned by
.IR from_list .
If the qualification is absent, the effect is to delete all instances
in the class.  The result is a valid, but empty class.
.PP
You must have write access to the class in order to modify it, as well
as read access to any class whose values are read in the qualification
(see
.IR "change acl" (l).
.SH EXAMPLE
.nf
--
--Remove all employees who make over $30,000
--
delete from emp where emp.sal > 30000
.fi
.nf
--
--Clear the hobbies class
--
delete from hobbies
.fi
.SH "SEE ALSO"
drop(l).
