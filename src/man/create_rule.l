.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_rule.l,v 1.11 1999/02/07 22:10:09 wieck Exp $
.TH "CREATE RULE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create rule - define a new rule
.SH SYNOPSIS
.nf
\fBcreate\fR \fBrule\fR rule_name
    \fBas\fR \fBon\fR event
      \fBto\fR object [\fBwhere\fR clause]
    \fBdo\fR [\fBinstead\fR]
    [\fBnothing\fP | action | \fB(\fPactions...\fB)\fP]
.fi
.SH DESCRIPTION
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
is a class name.
.PP
The 
.BR "where"
clause, and the
.IR action
are respectively normal SQL
.BR "where"
clauses and collections of SQL commands with the following change:
.IP
.BR new
or
.BR old
can appear instead of 
an instance variable whenever an instance 
variable is permissible in SQL.
.PP
Since v6.4 rules on
.IR select
are restricted to build
.BR views .
.BR "Create view"
should be used instead.
.PP
The semantics of a rule is that at the time an individual instance is
updated, inserted or deleted, there is an
.BR old
instance
(for updates and deletes) and a 
.BR new
instance (for updates and inserts).  If the event specified in the
.BR "on"
clause and the condition specified in the
.BR "where"
clause are true, then the 
.IR action
part of the rule is executed.  First, however, values from fields in
the old instance and/or the new instance are substituted for:
.nf
old.attribute-name
new.attribute-name
.fi
The
.IR action
part of the rule executes with same transaction identifier
before the user command that caused activation. 
.PP
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
It is very important to note that the 
.BR rewrite 
rule system will 
neither detect nor process circular
rules. For example, though each of the following two rule
definitions are accepted by Postgres, the  
.IR update 
command to one of the classes will cause 
Postgres to abort the transaction during the attempt to apply rules.
.nf

--
--Example of a circular rewrite rule combination. 
--
create rule bad_rule_combination_1 as
	on update to EMP 
	do update TOY set ...;

create rule bad_rule_combination_2 as
	on update to TOY
	do update EMP set ...;

.fi
.PP
You must have
.IR "rule definition"
access to a class in order to define a rule on it.
.PP
In contrast to queries run by trigger procedures,
the rule actions are executed under the permissions of the owner
of the 
.BR event
class. Thus, if the owner of a class defines a rule that inserts something
into another one (like in the log example below), the user updating the
.BR event
class must not have
.IR insert
permissions for the class specified in the
.BR "rule actions" .
This technique can safely be used to deny users from modifying event logging.
.SH EXAMPLES
.nf

--
--Make Sam get the same salary adjustment as Joe
--
create rule example_1 as
    on update to EMP where old.name = "Joe"
    do update EMP set salary = new.salary
        where EMP.name = "Sam";

.fi
At the time Joe receives a salary adjustment, the event will become
true and Joe's old instance and proposed new instance are available
to the execution routines.  Hence, his new salary is substituted into the 
.IR action
part of the rule which is executed.  This propagates
Joe's salary on to Sam.
.nf

--
-- Log changes to salary
--
create rule example_2 as
    on insert to EMP
    do insert into EMP_LOG (name, newsal, when)
        values (new.name, new.salary, 'now'::text);

create rule example_3 as
    on update to EMP where old.salary != new.salary
    do insert into EMP_LOG (name, oldsal, newsal, when)
        values (old.name, old.salary, new.salary, 'now'::text);

create rule example_4 as
    on delete to EMP
    do insert into EMP_LOG (name, oldsal, when)
        values (old.name, old.salary, 'now'::text);

.fi
.SH "SEE ALSO"
drop_rule(l),
create_view(l),
create_trigger(l).
.SH BUGS
.PP
The rule system stores the rule definition as query plans into text 
attributes.  This implies that creation of rules may fail if the
rule in its internal representations exceed some value
that is on the order of one page (8KB).
