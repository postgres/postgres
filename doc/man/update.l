.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/update.l,v 1.1.1.1 1996/08/18 22:14:28 scrappy Exp $
.TH UPDATE SQL 11/05/95 Postgres95 Postgres95
.SH NAME
update \(em replace values of attributes in a class
.SH SYNOPSIS
.nf
\fBupdate\fR classname \fBset\fR attname-1 = expression-1,
	{attname-i = expression-i}
	[\fBwhere\fR qual]
.fi
.SH DESCRIPTION
.BR Update
changes the values of the attributes specified in
.IR target_list 
for all instances which satisfy the qualification,
.IR qual .
Only the attributes to be modified need appear in
.IR target_list .
.PP
Array references use the same syntax found in 
.IR select(l).
That is, either single array elements, a range of array elements or
the entire array may be replaced with a single query.
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
