.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/sql.l,v 1.2.2.1 1996/12/11 00:32:04 momjian Exp $
.TH INTRODUCTION SQL 11/5/95 PostgreSQL PostgreSQL
.SH "Section 4 \(em SQL Commands (COMMANDS)"
.SH "General Information"
.SH DESCRIPTION
The following is a description of the general syntax of SQL.
Individual SQL statements and commands are treated separately in the
document; this section describes the syntactic classes from which the
constituent parts of SQL statements are drawn.
.SH Comments
A
.IR comment
is an arbitrary sequence of characters following double dashes up to the end
of the line e.g:
.nf
-- This is a comment
.fi
.SH "Names"
.IR Names
in SQL are sequences of not more than NAMEDATALEN alphanumeric characters,
starting with an alphabetic character.  By default, NAMEDATALEN is set
to 16, but at the time the system is built, NAMEDATALEN can be changed
by changing the #ifdef in src/backend/include/postgres.h.  Underscore
(\*(lq_\*(rq) is considered an alphabetic character. 
.SH "Keywords"
The following identifiers are reserved for use as
.IR keywords
and may not be used otherwise:
.PP
.ft B
.nf
.if n .ta 5 +15 +15 +15
.if t .ta 0.5i +1.5i +1.5i +1.5i
.fi
.ft
.ft B
.nf
.if n .ta 5 +15 +15 +15
.if t .ta 0.5i +1.5i +1.5i +1.5i
.fi
.ft
.PP
In addition, all Postgres classes have several predefined attributes used
by the system. 
.SH "Constants"
There are six types of
.IR constants
for use in SQL.  They are described below.
.SH "String Constants"
.IR Strings
in SQL are arbitrary sequences of ASCII characters bounded by single
quotes (' '). Uppercase alphabetics within strings are accepted
literally.  Non-printing characters may be embedded within strings by
prepending them with a backslash, e.g., `\en'. Also, in order to embed
quotes within strings, it is necessary to prefix them with `\e' .  The
same convention applies to `\e' itself.  Because of the limitations on
instance sizes, string constants are currently limited to a length of
a little less than 8192 bytes.  Larger objects may be created using the
Postgres Large Object interface.
.SH "Integer Constants"
.IR "Integer constants"
in SQL are collection of ASCII digits with no decimal point.  Legal
values range from \(mi2147483647 to +2147483647.  This will vary
depending on the operating system and host machine.
.SH "Floating Point Constants"
.IR "Floating point constants"
consist of an integer part, a decimal point, and a fraction part or
scientific notation of the following format:
.nf
{<dig>} .{<dig>} [e [+-] {<dig>}]
.fi
Where <dig> is a digit.  You must include at least one <dig> after the
period and after the [+-] if you use those options.  An exponent with
a missing mantissa has a mantissa of 1 inserted.  There may be no
extra characters embedded in the string.  
Floating point constaints are of type float4.
.SH "Constants of Postgres User-Defined Types"
A constant of an
.IR arbitrary
type can be entered using the notation:
.nf
'string'::type-name
.fi
or
.nf
CAST 'string' AS type-name
.fi
The value inside the string is passed to the input
conversion routine for the type called type-name. The result is a
constant of the indicated type.  The explicit typecast may be omitted
if there is no ambiguity as to the type the constant must be, in which
case it is automatically coerced.
.SH "Array constants"
.IR "Array constants"
are arrays of any Postgres type, including other arrays, string
constants, etc.  The general format of an array constant is the
following:
.nf
{<val1><delim><val2><delim>}
.fi
Where
.IR "<delim>"
is the delimiter for the type stored in the \*(lqpg_type\*(rq class.
(For built-in types, this is the comma character, \*(lq,\*(rq.)  An
example of an array constant is
.nf
{{1,2,3},{4,5,6},{7,8,9}}
.fi
This constant is a two-dimensional, 3 by 3 array consisting of three
sub-arrays of integers.
.PP
Individual array elements can and should be placed between quotation
marks whenever possible to avoid ambiguity problems with respect to
leading white space.
.\"  Elements of single-element arrays (e.g.,
.\" \*(lq{"1"}\*(rq) must be quoted.
.PP
.SH "FIELDS AND COLUMNS"
.SH "Fields"
A 
.IR field
is either an attribute of a given class or one of the following:
.nf
oid
tmin
tmax
xmin
xmax
cmin
cmax
.fi
.PP
.IR Oid
stands for the unique identifier of an instance which is added by
Postgres to all instances automatically. Oids are not reused and are 32
bit quantities.
.PP
.IR "Tmin, tmax, xmin, cmin, xmax"
and
.IR cmax
stand respectively for the time that the instance was inserted, the
time the instance was deleted, the identity of the inserting
transaction, the command identifier within the transaction, the
identity of the deleting transaction and its associated deleting
command.  For further information on these fields consult [STON87].
Times are represented internally as instances of the \*(lqabstime\*(rq
data type.  Transaction identifiers are 32 bit quantities which are
assigned sequentially starting at 512.  Command identifiers are 16 bit
objects; hence, it is an error to have more than 65535 SQL commands
within one transaction.
.SH "Columns"
A
.IR column
is a construct of the form:
.nf
Instance-variable{.composite_field}.field `['number`]'
.fi
.IR Instance-variable 
identifies a particular class and can be thought of as standing for
the instances of that class.  An instance variable is either a class
name, a surrogate for a class defined by means of a
.IR from
clause, or the keyword 
.BR new
or 
.BR current.
New and current can only appear in the action portion of a rule, while
other instance variables can be used in any SQL statement.
.IR Composite_field
is a field of of one of the Postgres composite types indicated in the 
.IR information (l)
section, while successive composite fields address attributes in the
class(s) to which the composite field evaluates.  Lastly,
.IR field
is a normal (base type) field in the class(s) last addressed.  If
.IR field 
is of type array, then the optional
.IR number 
designator indicates a specific element in the array.  If no number is
indicated, then all array elements are returned.
.SH "Operators"
Any built-in system, or user-defined operator may be used in SQL.
For the list of built-in and system operators consult
.BR "introduction" "(3)."
For a list of user-defined operators consult your system administrator
or run a query on the pg_operator class.  Parentheses may be used for
arbitrary grouping of operators.
.SH "Expressions (a_expr)"
An
.IR expression
is one of the following:
.nf
( a_expr )
constant
attribute
a_expr binary_operator a_expr
a_expr right_unary_operator
left_unary_operator a_expr
parameter
functional expressions 
aggregate expressions
.fi
We have already discussed constants and attributes.  The two kinds of
operator expressions indicate respectively binary and left_unary
expressions.  The following sections discuss the remaining options.
.SH "Parameters"
A 
.IR parameter
is used to indicate a parameter in a SQL function.  Typically this
is used in SQL function definition statement.  The form of a
parameter is:
.nf
\'$' number
.fi
For example, consider the definition of a function, DEPT, as
.nf
create function DEPT (char16) 
	returns dept
	as 'select * from 
	    dept where name=$1'
	language 'sql'
.fi
.SH "Functional Expressions"
A
.IR "functional expression"
is the name of a legal SQL function, followed by its argument list
enclosed in parentheses, e.g.:
.nf
fn-name (a_expr{ , a_expr})
.fi
For example, the following computes the square root of an employee
salary.
.nf
sqrt(emp.salary)
.fi
.SH "Aggregate Expression"
An
.IR "aggregate expression"
represents a simple aggregate (i.e., one that computes a single value)
or an aggregate function (i.e., one that computes a set of values).
The syntax is the following:
.nf
aggregate.name (attribute)
.fi
Here, 
.IR aggregate_name 
must be a previously defined aggregate.
.SH "Target_list"
A
.IR "target list"
is a parenthesized, comma-separated list of one or more elements, each
of which must be of the form:
.nf
a_expr[AS result_attname]
.fi
Here, result_attname is the name of the attribute to be created (or an
already existing attribute name in the case of update statements.)  If
.IR result_attname
is not present, then 
.IR a_expr
must contain only one attribute name which is assumed to be the name
of the result field.  In Postgres default naming is only used if
.IR a_expr
is an attribute.
.SH‚‚ "Qualification"
A 
.IR qualification 
consists of any number of clauses connected by the logical operators:
.nf
not
and
or
.fi
A clause is an 
.IR a_expr
that evaluates to a Boolean over a set of instances.
.SH "From List"
The 
.IR "from list"
is a comma-separated list of 
.IR "from expressions" .
.PP
Each 
.IR "from expression"
is of the form:
.nf
[class_reference] instance_variable
	{, [class_ref] instance_variable...}
.fi
where 
.IR class_reference
is of the form
.nf
class_name [time_expression] [*]
.fi
The 
.IR "from expression"
defines one or more instance variables to range over the class
indicated in 
.IR class_reference .
Adding a 
.IR time_expression
will indicate that a historical class is desired.  One can also request 
the instance variable to range over all classes that are beneath the
indicated class in the inheritance hierarchy by postpending the
designator \*(lq*\*(rq.
.SH‚‚ "Time Expressions"
A
.IR "time expression"
is in one of two forms:
.nf
 ["date"]
 ["date-1", "date-2"]
.fi
The first case requires instances that are valid at the indicated
time.  The second case requires instances that are valid at some time
within the date range specified.  If no time expression is indicated,
the default is \*(lqnow\*(rq.
.PP
In each case, the date is a character string of the form
.nf
"[MON-FRI] MMM DD [HH:MM:SS] YYYY [Timezone]"
.fi
where MMM is the month (Jan \- Dec), DD is a legal day number in the
specified month, HH:MM:SS is an optional time in that day (24-hour
clock), and YYYY is the year.  If the time of day HH:MM:SS is not
specified, it defaults to midnight at the start of the specified day.
As of Version 3.0, times are no longer read and written using
Greenwich Mean Time; the input and output routines default to the
local time zone.
.PP
For example,
.nf
["Jan 1 1990"]
["Mar 3 00:00:00 1980", "Mar 3 23:59:59 1981"]
.fi
are valid time specifications.
.PP
Note that this syntax is slightly different than that used by the 
time-range type.
.SH "SEE ALSO"
insert(l),
delete(l),
execute(l),
update(l),
select(l),
monitor(1).
