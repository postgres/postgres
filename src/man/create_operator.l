.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_operator.l,v 1.8 1999/04/15 00:09:00 tgl Exp $
.TH "CREATE OPERATOR" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create operator - define a new user operator
.SH SYNOPSIS
.nf
\fBcreate operator\fR operator_name
	\fB(\fR[ \fBleftarg\fR \fB=\fR type-1 ]
	 [ \fB,\fR \fBrightarg\fR \fB=\fR type-2 ]
	 , \fBprocedure =\fR func_name
	 [\fB, commutator =\fR com_op ]
	 [\fB, negator =\fR neg_op ]
	 [\fB, restrict =\fR res_proc ]
	 [\fB, join =\fR join_proc ]
	 [\fB, hashes\fR]
	 [\fB, sort1 =\fR left_sort_op ]
	 [\fB, sort2 =\fR right_sort_op ]
	\fB)\fR
.\" \fB"arg is ("
.\" type [
.\" \fB,
.\" type ]
.\" \fB)
.fi
.SH DESCRIPTION
This command defines a new user operator,
.IR "operator_name" .
The user who defines an operator becomes its owner.
.PP
The
.IR "operator_name"
is a sequence of punctuation characters.  The following
characters are valid for single-character operator names:
.nf

.ce 1
~ ! @ # % ^ & ` ?

.fi
If the operator name is more than one character long, it may consist
of any combination of the above characters or the following additional
characters:
.nf

.ce 1
| $ : + - * / < > =

.fi
The operator "!=" is mapped to "<>" on input, and they are
therefore equivalent.
.PP
At least one of
.IR leftarg
and
.IR rightarg
must be defined.  For binary operators, both should be defined. For
right unary operators, only
.IR arg1
should be defined, while for left unary operators only
.IR arg2
should be defined.
.PP
The name of the operator,
.IR operator_name ,
can be composed of symbols only.  Also, the
.IR func_name
procedure must have been previously defined using
.IR create_function(l)
and must have one or two arguments.
.PP
.\" that multiple instances of the 
.\" operator must be be evaluated
.\" For example, consider the area-intersection operator,
.\" .q A,
.\" and the following expression:
.\" .(l
.\" MYBOXES2.description A \*(lq0,0,1,1\*(rq A MYBOXES.description
.\" .)l
.\" .in .5i
.\" The associativity flag indicates that
.\" .(l
.\" (MYBOXES2.description A \*(lq0,0,1,1\*(rq) A MYBOXES.description
.\" .)l
.\" .in .5i
.\" is the same as
.\" .(l
.\" MYBOXES2.description A (\*(lq0,0,1,1\*(rq A MYBOXES.description).
.\" .)l
The commutator operator is present so that Postgres can reverse the order
of the operands if it wishes.  For example, the operator
area-less-than, >>>, would have a commutator operator,
area-greater-than, <<<.  Hence, the query optimizer
could freely convert:
.nf

.ce 1
"0,0,1,1"::box >>> MYBOXES.description

.fi
to
.nf

.ce 1
MYBOXES.description <<< "0,0,1,1"::box

.fi
This allows the execution code to always use the latter representation
and simplifies the query optimizer somewhat.
.PP
Suppose that an operator, area-equal, ===,
exists, as well as an area not equal, !==.
The negator operator allows the query optimizer to convert
.nf

.ce 1
NOT MYBOXES.description === "0,0,1,1"::box

.fi
to
.nf

.ce 1
MYBOXES.description !== "0,0,1,1"::box

.fi
If a commutator operator name is supplied, Postgres searches for it in
the catalog.  If it is found and it does not yet have a commutator
itself, then the commutator's entry is updated to have the newly created
operator as its commutator.  This applies to the negator, as well.
.PP
This is to allow the definition of two operators that are the
commutators or the negators of each other.  The first operator should
be defined without a commutator or negator (as appropriate).  When the
second operator is defined, name the first as the commutator or
negator.  The first will be updated as a side effect.  (As of Postgres 6.5,
it also works to just have both operators refer to each other.)
.PP
The next three specifications are present to support the query optimizer
in performing joins.  Postgres can always evaluate a join (i.e.,
processing a clause with two tuple variables separated by an operator
that returns a boolean) by iterative substitution [WONG76].  In
addition, Postgres can use a hash-join algorithm
along the lines of [SHAP86]; however, it must know whether this
strategy is applicable.
The current hash-join algorithm
is only correct for operators that represent equality tests;
furthermore, equality of the datatype must mean bitwise equality
of the representation of the type.  (For example, a datatype that
contains unused bits that don't matter for equality tests could
not be hashjoined.)
The
.BR hashes
flag indicates to the query optimizer that a hash join may safely be
used with this operator.
.PP
Similarly, the two sort operators indicate to the query optimizer
whether merge-sort is a usable join strategy and which operators should
be used to sort the two operand classes.
Sort operators should only be provided for an equality
operator, and they should refer to less-than operators for the
left and right side data types respectively.
.PP
If other join strategies are found to be practical, Postgres will change
the optimizer and run-time system to use them and will require
additional specification when an operator is defined.  Fortunately,
the research community invents new join strategies infrequently, and
the added generality of user-defined join strategies was not felt to
be worth the complexity involved.
.PP
The last two pieces of the specification are present so the query
optimizer can estimate result sizes.  If a clause of the form:
.nf

.ce 1
MYBOXES.description <<< "0,0,1,1"::box

.fi
is present in the qualification, then Postgres may have to estimate the
fraction of the instances in MYBOXES that satisfy the clause.  The
function res_proc must be a registered function (meaning it is already
defined using
.IR create_function(l))
which accepts one argument of the correct data type and returns a
floating point number.  The query optimizer simply calls this
function, passing the parameter "0,0,1,1"
and multiplies the result by the relation size to get the desired
expected number of instances.
.PP
Similarly, when the operands of the operator both contain instance
variables, the query optimizer must estimate the size of the resulting
join.  The function join_proc will return another floating point
number which will be multiplied by the cardinalities of the two
classes involved to compute the desired expected result size.
.PP
The difference between the function
.nf

.ce 1
my_procedure_1 (MYBOXES.description, "0,0,1,1"::box)

.fi
and the operator
.nf

.ce 1
MYBOXES.description === "0,0,1,1"::box

.fi
is that Postgres attempts to optimize operators and can decide to use an
index to restrict the search space when operators are involved.
However, there is no attempt to optimize functions, and they are
performed by brute force.  Moreover, functions can have any number of
arguments while operators are restricted to one or two.
.SH EXAMPLE
.nf
--
--The following command defines a new operator,
--area-equality, for the BOX data type.
--
create operator === (
	leftarg = box,
	rightarg = box,
	procedure = area_equal_procedure,
	commutator = ===,
	negator = !==,
	restrict = area_restriction_procedure,
	join = area_join_procedure,
	hashes,
	sort1 = <<<,
	sort2 = <<<)
.\"	arg is (box, box)
.fi
.SH "SEE ALSO"
create_function(l),
drop_operator(l).
.SH BUGS
Operator names cannot be composed of alphabetic characters in 
Postgres.
.PP
If an operator is defined before its commuting operator has been defined,
a dummy entry for the commutator (with invalid oprproc field) will be placed
in the system catalogs.  This entry will be overridden when the commutator
is eventually defined.
