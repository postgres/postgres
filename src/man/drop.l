.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop.l,v 1.2 1996/12/11 00:27:37 momjian Exp $
.TH "DROP TABLE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop table \(em destroy existing classes
.SH SYNOPSIS
.nf
\fBdrop table\fR classname-1 { \fB,\fR classname-i }
.fi
.SH DESCRIPTION
.BR "Drop Table"
removes classes from the data base.  Only its owner may destroy a
class.  A class may be emptied of instances, but not destroyed, by
using 
.IR delete (l).
.PP
If a class being destroyed has secondary indices on it, then they will
be removed first.  The removal of just a secondary index will not
affect the indexed class.
.PP
The destruction of classes is not reversable.  Thus, a destroyed class
will not be recovered if a transaction which destroys this class fails
to commit.  In addition, historical access to instances in a destroyed
class is not possible.
.SH EXAMPLE
.nf
--
--Destroy the emp class
--
drop table emp
.fi
.nf
--
--Destroy the emp and parts classes
--
drop table emp, parts
.fi
.SH "SEE ALSO"
delete(l),
drop index(l).
