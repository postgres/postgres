.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/insert.l,v 1.1 1996/11/14 10:17:19 scrappy Exp $
.TH INSERT SQL 11/05/95 Postgres95 Postgres95
.SH NAME
insert \(em insert tuples to a relation
.SH SYNOPSIS
.nf
\fBinsert\fR into classname
    [(att.expr-1,{att_expr.i})]
	{\fBvalues\fR (expression1 {,expression-i}) |
    \fBselect\fR expression1,{expression-i}
	[\fBfrom\fR from-list] [\fBwhere\fR qual] 
.fi
.SH DESCRIPTION
.BR Insert
adds instances that satisfy the qualification,
.IR qual ,
to
.IR classname .
.IR Classname 
must be the name of an existing class.  The target list specifies the
values of the fields to be appended to
.IR classname .
That is, each 
.IR att_expr
specifies a field (either an attribute name or an attribute name plus
an array specification) to which the corresponding 
.IR expression
should be assigned.  The fields in the target list may be listed in
any order.  Fields of the result class which do not appear in the
target list default to NULL.  If the expression for each field is not
of the correct data type, automatic type coercion will be attempted.
.PP
An array initialization may take exactly one of the following forms:
.nf
--
-- Specify a lower and upper index for each dimension 
-- 
att_name[lIndex-1:uIndex-1]..[lIndex-i:uIndex-i] = array_str

--
--Specify only the upper index for each dimension
--(each lower index defaults to 1)
--
att_name[uIndex-1]..[uIndex-i] = array_str

--
--Use the upper index bounds as specified within array_str
--(each lower index defaults to 1)
--
att_name = array_str
.fi
where each
.IR lIndex
or
.IR uIndex
is an integer constant and
.IR array_str
is an array constant (see
.IR introduction (l)).
.PP

If the user does not specify any array bounds (as in the third form)
then Postgres will attempt to deduce the actual array bounds from the
contents of
.IR array_str .

If the user does specify explicit array bounds (as in the first and
second forms) then the array may be initialized partly or fully 
using a C-like syntax for array initialization. 
However, the uninitialized array elements will
contain garbage.
.PP
You must have write or append access to a class in order to append to
it, as well as read access on any class whose values are read in the
target list or qualification (see
.IR "change acl" (l)).
.SH EXAMPLES
.nf
--
--Make a new employee Jones work for Smith
--
insert into emp
    select newemp.name, newemp.salary,
	"Smith", 1990-newemp.age
	from newemp
	where name = "Jones"
.fi
.nf
--
--Insert into newemp class to newemp
--
insert into newemp
    select * from newemp1 
.fi
.nf
--
--Create an empty 3x3 gameboard for noughts-and-crosses
--(all of these queries create the same board attribute)
--
insert into tictactoe (game, board[1:3][1:3])
    values(1,'{{"","",""},{},{"",""}}')
insert into tictactoe (game, board[3][3])
    values (2,'{}')
insert into tictactoe (game, board) 
    values (3,'{{,,},{,,},{,,}}')
.fi
.SH "SEE ALSO"
create table(l),
create type(l),
update(l),
select(l)
