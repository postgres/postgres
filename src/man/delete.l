.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/delete.l,v 1.2 1996/12/11 00:27:26 momjian Exp $
.TH DELETE SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
delete \(em delete instances from a class
.SH SYNOPSIS
.nf
\fBdelete\fR \fBfrom\fR class_name [ \fBwhere\fR qual ]
.fi
.SH DESCRIPTION
.BR Delete
removes instances which satisfy the qualification,
.IR qual
from the specified class.
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
