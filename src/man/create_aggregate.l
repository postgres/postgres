.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_aggregate.l,v 1.5 1998/01/11 22:17:11 momjian Exp $
.TH "CREATE AGGREGATE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create aggregate - define a new aggregate
.SH SYNOPSIS
.nf
\fBcreate aggregate\fR agg-name [\fBas\fR]
	\fB(\fP[\fBsfunc1\fR \fB=\fR state-transition-function-1
	  ,\fP \fBbasetype\fR \fB=\fR data-type
	  ,\fP \fBstype1\fR \fB=\fR sfunc1-return-type]
	 [\fB,\fP \fBsfunc2\fR \fB=\fR state-transition-function-2
	  ,\fP \fBstype2\fR \fB=\fR sfunc2-return-type]
	 [\fB,\fP \fBfinalfunc\fR \fB=\fR final-function]
	 [\fB,\fP \fBinitcond1\fR \fB=\fR initial-condition-1]
	 [\fB,\fP \fBinitcond2\fR \fB=\fR initial-condition-2]\fB)\fR
.fi
.SH DESCRIPTION
An aggregate function can use up to three functions, two
.IR "state transition"
functions, X1 and X2:
.nf
X1( internal-state1, next-data_item ) ---> next-internal-state1
X2( internal-state2 ) ---> next-internal-state2
.fi
and a
.BR "final calculation"
function, F:
.nf
F(internal-state1, internal-state2) ---> aggregate-value
.fi
These functions are required to have the following properties:
.IP
The arguments to state-transition-function-1 must be
.BR ( stype1 , basetype ) ,
and its return value must be stype1.
.IP
The argument and return value of state-transition-function-2 must be
.BR stype2 .
.IP
The arguments to the final-calculation-function must be
.BR ( stype1 , stype2 ) ,
and its return value must be a POSTGRES base type (not
necessarily the same as basetype.
.IP
The final-calculation-function should be specified if and only if both
state-transition functions are specified.
.PP
Note that it is possible to specify aggregate functions that have
varying combinations of state and final functions.  For example, the
\*(lqcount\*(rq aggregate requires
.BR sfunc2
(an incrementing function) but not
.BR sfunc1 " or " finalfunc ,
whereas the \*(lqsum\*(rq aggregate requires
.BR sfunc1
(an addition function) but not
.BR sfunc2 " or " finalfunc
and the \*(lqaverage\*(rq aggregate requires both of the above state
functions as well as a
.BR finalfunc
(a division function) to produce its answer.  In any case, at least
one state function must be defined, and any
.BR sfunc2
must have a corresponding 
.BR initcond2 .
.PP
Aggregates also require two initial conditions, one for each
transition function.  These are specified and stored in the database
as fields of type
.IR text .
.SH EXAMPLE
This
.IR avg
aggregate consists of two state transition functions, a addition
function and a incrementing function.  These modify the internal state
of the aggregate through a running sum and and the number of values
seen so far.  It accepts a new employee salary, increments the count,
and adds the new salary to produce the next state.  The state
transition functions must be passed correct initialization values.
The final calculation then divides the sum by the count to produce the
final answer.
.nf
--
--Create an aggregate for int4 average
--
create aggregate avg (sfunc1 = int4add, basetype = int4,
     stype1 = int4, sfunc2 = int4inc, stype2 = int4,
     finalfunc = int4div, initcond1 = "0", initcond2 = "0")
.fi
.SH "SEE ALSO"
create function(l),
remove aggregate(l).
