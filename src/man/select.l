.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/select.l,v 1.2 1996/12/11 00:28:09 momjian Exp $
.TH SELECT SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
select \(em retrieve instances from a class
.SH SYNOPSIS
.nf
\fBselect\fR [distinct]
    expression1 [\fBas\fR attr_name-1]
    {, expression-1 [\fBas\fR attr_name-i]}
    [\fBinto\fR \fBtable\fR classname]
    [\fBfrom\fR from-list]
    [\fBwhere\fR where-clause]    
    [\fBgroup by\fR attr_name1 {, attr_name-i....}
    [\fBorder by\fR attr_name1
	[\fBusing op1\fR] {, attr_namei [\fBusing opi\fR] }
.fi
.SH DESCRIPTION
.BR Select
will get all instances which satisfy the qualification,
.IR qual ,
compute the value of each element in the target list, and either (1)
return them to an application program through one of two different
kinds of portals or (2) store them in a new class.
.PP
If
into table class name
is specified, the result of the query will be stored in a new class
with the indicated name.
.PP
The
.BR "order by"
clause allows a user to specify that he wishes the instances sorted
according to the corresponding operator.  This operator must be a
binary one returning a boolean.  Multiple sort fields are allowed and
are applied from left to right.
.PP
The target list specifies the fields to be retrieved.  Each 
.IR attr_name
specifies the desired attribute or portion of an array attribute.
Thus, each 
.IR attr_name
takes the form
.nf
class_name.att_name
.fi
or, if the user only desires part of an array,
.nf
--
--Specify a lower and upper index for each dimension
--(i.e., clip a range of array elements)
--
class_name.att_name[lIndex-1:uIndex-1]..[lIndex-i:uIndex-i]

--
--Specify an exact array element
--
class_name.att_name[uIndex-1]..[uIndex-i]
.fi
where each 
.IR lIndex
or
.IR uIndex
is an integer constant.
.PP
When you retrieve an attribute which is of a complex type, the behavior
of the system depends on whether you used "nested dots" to project
out attributes of the complex type or not.  See the examples below.
.PP
You must have read access to a class to read its values (see
.IR "grant/revoke" (l).
.SH EXAMPLES
.nf
--
--Find all employees who make more than their manager
--
select e.name
   from emp e, emp m
   where e.mgr = m.name
   and e.sal >  m.sal
.fi
.nf
--
--Retrieve all fields for those employees who make
--more than the average salary
--
select avg(sal) as ave 
   into table avgsal from emp;
.fi
.nf
--
--Retrieve all employee names in sorted order
--
select distinct name
   from emp
   order by name using <
.fi
.nf
--
--Retrieve all employee names that were valid on 1/7/85 
--in sorted order
--
selec name
   from emp['January 7 1985'] e
   order by name using < 
.fi
.nf
--
--Construct a new class, raise, containing 1.1
--times all employee's salaries
--
select 1.1 * emp.salary as salary
    into tables raise
    from emp
.fi
.SH "SEE ALSO"
insert(l),
close(l),
create table(l),
fetch(l),
update(l).
.SH BUGS
.BR "Select into"
does not delete duplicates.
.PP
If the backend crashes in the course of executing a 
.BR "select into" ,
the class file will remain on disk.  It can be safely removed by the
database DBA, but a subsequent
.BR "select into"
to the same name will fail with a cryptic error message about
\*(lqBlockExtend\*(rq.
