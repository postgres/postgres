.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/alter_table.l,v 1.5 1998/03/31 04:44:19 momjian Exp $
.TH "ALTER TABLE" SQL 09/25/97 PostgreSQL
.SH NAME
alter table - add attributes to a class, or rename an attribute or class
.SH SYNOPSIS
.nf
\fBalter table\fR classname [ * ]
	\fBadd\fR [ \fBcolumn\fR ] attname type
\fBalter table\fR classname [ * ]
	\fBadd\fR \fB(\fR attname type \fB)\fR
	
\fBalter table\fR classname1
	\fBrename to\fR classname2

\fBalter table\fR classname1 [\fB*\fR]
	\fBrename [column]\fR attname1 \fBto\fR attname2
.fi
        
.SH DESCRIPTION
The
.BR "alter table"
command causes a new attribute to be added to an existing class,
.IR classname ,
or the name of a class or attribute to change
without changing any of the data contained in the affected class.
Thus, the class or attribute will remain of the same type and size
after this command is executed.
.PP
The new attributes and their types are specified
in the same style and with the the same restrictions as in
.IR "create table" (l).
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
In order to rename an attribute in each class in an entire inheritance
hierarchy, use the 
.IR classname
of the superclass and append a \*(lq*\*(rq.  (By default, the attribute
will not be renamed in any of the subclasses.)  This should
.BR always
be done when changing an attribute name in a superclass.  If it is
not, queries on the inheritance hierarchy such as
.nf
select * from super* s
.fi
will not work because the subclasses will be (in effect) missing an
attribute found in the superclass.
.PP
You must own the class being modified in order to rename it or part of
its schema.  Renaming any part of the schema of a system catalog is
not permitted.
.PP
You must own the class in order to change its schema.
.SH EXAMPLE
.nf
--
-- add the date of hire to the emp class
-- 
alter table emp add column hiredate abstime
--
-- add a health-care number to all persons
-- (including employees, students, ...)
--
alter table person * add column health_care_id int4
--
-- change the emp class to personnel
--
alter table emp rename to personnel
--
-- change the sports attribute to hobbies
--
alter table emp rename column sports to hobbies
--
-- make a change to an inherited attribute
--
alter table person * rename column last_name to family_name
.fi
.SH "SEE ALSO"
create table (l),
update (l).

