.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/rename.l,v 1.2 1996/12/11 00:28:06 momjian Exp $
.TH RENAME SQL 02/08/94 PostgreSQL PostgreSQL
.SH NAME
rename \(em rename a class or an attribute in a class
.SH SYNOPSIS
.nf
\fBalter table\fR classname1
        \fBrename to\fR classname2
\fBalter table\fR classname1 [\fB*\fR]
        \fBrename [column]\fR attname1 \fBto\fR attname2
.fi
.SH DESCRIPTION
The
.BR rename
command
causes the name of a class or attribute to change without changing any
of the data contained in the affected class.  Thus, the class or
attribute will remain of the same type and size after this command is
executed.
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
.SH EXAMPLE
.nf
--
-- change the emp class to personnel
--
alter table emp rename to personnel
.fi
.nf
--
-- change the sports attribute to hobbies
--
alter table emp rename column sports to hobbies
.fi
.nf
--
-- make a change to an inherited attribute
--
alter table person * rename column last_name to family_name
.fi
.SH BUGS
Execution of historical queries using classes and attributes whose
names have changed will produce incorrect results in many situations.
.PP
Renaming of types, operators, rules, etc., should also be supported.
