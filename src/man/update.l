.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/update.l,v 1.1.2.1 1996/12/11 00:32:07 momjian Exp $
.TH UPDATE SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
update \(em replace values of attributes in a class
.SH SYNOPSIS
.nf
\fBupdate\fR classname \fBset\fR attname-1 = expression-1,
	{attname-i = expression-i}
	[\fBfrom\fR from-list]
	[\fBwhere\fR qual]
.fi
.SH DESCRIPTION
.BR Update
changes the values of the attributes specified
for all instances which satisfy the qualification,
.IR qual .
Only the attributes to be modified need appear as atributes.
.PP
Array references use the same syntax found in 
.IR select(l).
That is, either single array elements, a range of array elements or
the entire array may be replaced with a single query.
.IR from-list
is a non-standard extension to allow columns from
other tables to appear in the target_list.
.PP
You must have write access to the class in order to modify it, as well
as read access to any class whose values are mentioned in the target list
or qualification.
.SH EXAMPLES
.nf
--
--Give all employees who work for Smith a 10% raise
--
update emp 
   set sal = 1.1 * sal
   where mgr = 'Smith'
.fi
.SH "SEE ALSO"
create table(l),
select(l).
