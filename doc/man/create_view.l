.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/create_view.l,v 1.1.1.1 1996/08/18 22:14:22 scrappy Exp $
.TH "CREATE VIEW" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
create view \(em construct a virtual class
.SH SYNOPSIS
.nf
\fBcreate view\fR view_name \fBas\fR
	\fBselect\fR expression1 [\fBas\fR attr_name1]
	{, expression_i [\fBas\fR attr_namei]}
	[\fBfrom\fR from.last]
	[\fBwhere\fR qual]
.fi
.SH DESCRIPTION
.BR "create view"
will define a view of a class.  This view is not physically
materialized; instead the rule system is used to support view
processing as in [STON90].  Specifically, a query rewrite retrieve
rule is automatically generated to support retrieve operations on
views.  Then, the user can add as many update rules as desired to
specify the processing of update operations to views.  See [STON90]
for a detailed discussion of this point.
.SH EXAMPLE
.nf
--
--create a view consisting of toy department employees
--
create view toyemp as
  	select e.name
	from emp e
	where e.dept = 'toy'
.fi
.nf
--
--Specify deletion semantics for toyemp
--
create rule example1 as
	on delete to toyemp
	do instead delete emp
	where emp.oid = current.oid
.fi
.SH "SEE ALSO"
create table(l), 
create rule(l),
