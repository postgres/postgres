.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/alter_table.l,v 1.1.2.1 1996/12/11 00:30:47 momjian Exp $
.TH "ALTER TABLE" SQL 11/5/95 PostgreSQL PostgreSQL
.SH NAME
alter table \(em add attributes to a class
.SH SYNOPSIS
.nf
\fBalter table\fR classname [*]
	\fBadd column\fR attname type
	
.fi
.SH DESCRIPTION
The
.BR "alter table"
command
causes a new attribute to be added to an existing class,
.IR classname .
The new attributes and their types are specified
in the same style and with the the same restrictions as in
.IR create table (l).
.PP
In order to add an attribute to each class in an entire inheritance
hierarchy, use the
.IR classname
of the superclass and append a \*(lq*\*(rq.  (By default, the
attribute will not be added to any of the subclasses.)  This should
.BR always
be done when adding an attribute to a superclass.  If it is not,
queries on the inheritance hierarchy such as
.nf
select * from super* s
.fi
will not work because the subclasses will be missing an attribute
found in the superclass.
.PP
For efficiency reasons, default values for added attributes are not
placed in existing instances of a class.  That is, existing instances
will have NULL values in the new attributes.  If non-NULL values are
desired, a subsequent
.IR update (l)
query should be run.
.PP
You must own the class in order to change its schema.
.SH EXAMPLE
.nf
--
-- add the date of hire to the emp class
-- 
alter table emp add column hiredate abstime
.fi
.nf
--
-- add a health-care number to all persons
-- (including employees, students, ...)
--
alter table person * add column health_care_id int4
.fi
.SH "SEE ALSO"
create table (l),
update (l).
