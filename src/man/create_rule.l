.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_rule.l,v 1.1.2.1 1996/12/11 00:31:09 momjian Exp $
.TH "CREATE RULE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create rule \(em define a new rule
.SH SYNOPSIS
.nf
\fBcreate\fR \fBrule\fR rule_name
    \fBas\fR \fBon\fR event
      \fBto\fR object [\fBwhere\fR clause]
    \fBdo\fR [\fBinstead\fR]
    [action | nothing | \fB[\fPactions...\fB]\fP]
.fi
.SH DESCRIPTION
.IR "The current rule system implementation is very brittle and is unstable.  Users are discouraged from using rules at this time."
.PP
.BR "Create rule"
is used to define a new rule.
.PP
Here, 
.IR event
is one of 
.IR select ,
.IR update ,
.IR delete
or
.IR insert .
.IR Object
is either:
.nf
a class name
    \fIor\fR
class.column
.fi
The 
.BR "from"
clause, the 
.BR "where"
clause, and the
.IR action
are respectively normal SQL
.BR "from"
clauses,
.BR "where"
clauses and collections of SQL commands with the following change:
.IP
.BR new
or
.BR current
can appear instead of 
an instance variable whenever an instance 
variable is permissible in SQL.
.PP
The semantics of a rule is that at the time an individual instance is
accessed, updated, inserted or deleted, there is a 
.BR current
instance
(for retrieves, updates and deletes) and a 
.BR new
instance (for updates and appends).  If the event specified in the
.BR "on"
clause and the condition specified in the
.BR "where"
clause are true for the current instance, then the 
.IR action
part of the rule is executed.  First, however, values from fields in
the current instance and/or the new instance are substituted for:
.nf
current.attribute-name
new.attribute-name
.fi
The
.IR action
part of the rule executes with same command and transaction identifier
as the user command that caused activation.
.PP
A note of caution about SQL rules is in order.  If the same class
name or instance variable appears in the event, 
.BR where
clause and the 
.IR action
parts of a rule, they are all considered different tuple variables.
More accurately,
.BR new
and
.BR current
are the only tuple variables that are shared between these clauses.
For example, the following two rules have the same semantics:
.nf
on update to EMP.salary where EMP.name = "Joe"
	do update EMP ( ... ) where ...

on update to EMP-1.salary where EMP-2.name = "Joe"
	do update EMP-3 ( ... ) where ...
.fi
Each rule can have the optional tag 
.BR "instead" .
Without this tag 
.IR action
will be performed in addition to the user command when the event in
the condition part of the rule occurs.  Alternately, the
.IR action 
part will be done instead of the user command.
In this later case, the action can be the keyword
.BR nothing .
.PP
When choosing between the rewrite and instance rule systems for a
particular rule application, remember that in the rewrite system
.BR current
refers to a relation and some qualifiers whereas in the instance
system it refers to an instance (tuple).
.PP
It is very important to note that the 
.BR rewrite 
rule system will 
neither detect nor process circular
rules. For example, though each of the following two rule
definitions are accepted by Postgres, the  
.IR retrieve 
command will cause 
Postgres to 
.IR crash :
.nf
--
--Example of a circular rewrite rule combination. 
--
create rule bad_rule_combination_1 is
	on select to EMP 
	do instead select to TOYEMP

create rule bad_rule_combination_2 is
	on select to TOYEMP
	do instead select to EMP

--
--This attempt to retrieve from EMP will cause Postgres to crash.
--
select * from EMP
.fi
.PP
You must have
.IR "rule definition"
access to a class in order to define a rule on it (see
.IR "change acl" (l).
.SH EXAMPLES
.nf
--
--Make Sam get the same salary adjustment as Joe
--
create rule example_1 is
    on update EMP.salary where current.name = "Joe"
    do update EMP (salary = new.salary)
	where EMP.name = "Sam"
.fi
At the time Joe receives a salary adjustment, the event will become
true and Joe's current instance and proposed new instance are available
to the execution routines.  Hence, his new salary is substituted into the 
.IR action
part of the rule which is subsequently executed.  This propagates
Joe's salary on to Sam.
.nf
--
--Make Bill get Joe's salary when it is accessed
--
create rule example_2 is
    on select to EMP.salary
        where current.name = "Bill"
    do instead
	select (EMP.salary) from EMP where EMP.name = "Joe"
.fi
.nf
--
--Deny Joe access to the salary of employees in the shoe
--department.  (pg_username() returns the name of the current user)
--
create rule example_3 is
    on select to EMP.salary
	where current.dept = "shoe"
              and pg_username() = "Joe"
    do instead nothing
.fi
.nf
--
--Create a view of the employees working in the toy department.
--
create TOYEMP(name = char16, salary = int4)

create rule example_4 is
    on select to TOYEMP
    do instead select (EMP.name, EMP.salary) from EMP
	where EMP.dept = "toy"
.fi
.nf
--
--All new employees must make 5,000 or less
--
create rule example_5 is
	on insert to EMP where new.salary > 5000
	do update newset salary = 5000
.fi
.SH "SEE ALSO"
drop rule(l),
create view(l).
.SH BUGS
.PP
.BR "instead"
rules do not work properly.
.PP
The object in a SQL rule cannot be an array reference and cannot
have parameters.
.PP
Aside from the \*(lqoid\*(rq field, system attributes cannot be
referenced anywhere in a rule.  Among other things, this means that
functions of instances (e.g., \*(lqfoo(emp)\*(rq where \*(lqemp\*(rq
is a class) cannot be called anywhere in a rule.
.PP
The rule system store the rule text and query plans as text 
attributes.  This implies that creation of rules may fail if the
rule plus its various internal representations exceed some value
that is on the order of one page (8KB).
