.pl 27.0c
.ll 17.0c
.po 2.0c
.nf
.nh
.de HD
.sp 2m
..
.de FT
.sp 2m
.tl _PL/Tcl_A PostgreSQL PL_Page %
..
.wh 0 HD
.wh -3 FT
.sp 5m
.ce 1000
PL/Tcl
A procedural language for the

PostgreSQL
database system
.ce 0
.sp 5m
.fi
.in +4
PL/Tcl is a dynamic loadable extension for the PostgreSQL database system
that enables the Tcl language to be used to create functions and
trigger-procedures. It offers most of the capabilities a function
writer has in the C language, except for some restrictions.

The good restriction is, that everything is executed in a safe
Tcl-interpreter. In addition to the limited command set of safe Tcl, only
a few commands are available to access the database over SPI and to raise
messages via elog(). There is no way to access internals of the
database backend or gaining OS-level access under the permissions of the
PostgreSQL user ID like in C. Thus, any unprivileged user may be
permitted to use this language.

The other, internal given, restriction is, that Tcl procedures cannot
be used to create input-/output-functions for new data types.
.bp
.ti -4
Data type conversions

PostgreSQL has a rich set of builtin data types. And new data types can
be defined. The trick is, that PostgreSQL doesn't really know much about
the internals of a data type. It just offers a container for storing the
values and knows some functions to call to convert between the external
string representation and the internal container format. In addition, it
knows which functions to call to compare containers or to do some 
arithmetics on them for sorting, indexing and calculations.

Tcl on the other hand stores all values as strings.

These two different concepts meet perfectly for what we need. A PostgreSQL
function has a return value and up to 9 arguments. The data types appear
in the pg_type system catalog, where we find their type specific regproc's
responsible for input-/output-conversion from/to strings.

A special case are set values, which can appear as arguments to a
function. A set value is like a structure containing all the fields
of a table as it's elements. 

C functions cannot have sets as return values. So we cannot do this in
Tcl either.


.ti -4
PostgreSQL functions and Tcl procedure names

In PostgreSQL, one and the same function name can be used for
different functions as long as the number of arguments or their types
differ. This would collide with Tcl procedure names. To offer the same
flexibility in PL/Tcl, the internal Tcl procedure names contain the object
ID of the procedures pg_proc row as part of their name. Thus, different
argtype versions of the same PostgreSQL function are different for Tcl too.
.bp
.ti -4
Defining PostgreSQL functions in PL/Tcl

The following assumes, that the PL/Tcl language is created by the
administrator of the database with the language name 'pltcl'. See the
installation instructions to do that.

To create a function in the PL/Tcl language, use the known syntax:

.nf
    CREATE FUNCTION funcname ([typename [...]])
.in +4
    RETURNS typename AS '
.in +4
    PL/Tcl procedure body
.in -4
    ' LANGUAGE 'pltcl';
.in -4
.fi

When calling this function in a query, the arguments are given as
variables $1 ... $n to the procedure body. So a little max function
returning the higher of two int4 values would be created as:

.nf
    create function max (int4, int4)
.in +4
    returns int4 as '
.in +4
    if {$1 > $2} {return $1}
    return $2
.in -4
    ' language 'pltcl';
.in -4
.fi

Set arguments are given to the procedure as Tcl arrays. The element names
in the array are the field names of the set. If a field in the actual set
has the NULL value, it will not appear in the array! The overpaid_2 sample
from the CREATE FUNCTION section of the manual would be defined in Tcl as

.nf
    create function overpaid_2 (EMP)
.in +4
    returns bool as '
.in +4
    if {200000.0 < $1(salary)} {
.in +4
    return 't'
.in -4
    }
    if {$1(age) < 30 && 100000.0 < $1(salary)} {
.in +4
    return 't'
.in -4
    }
    return 'f'
.in -4
    ' language 'pltcl';
.in -4
.fi

Sometimes (especially when using the SPI functions described later) it
is useful to have some global status data that is held between two
calls to a procedure. To protect PL/Tcl procedures from side effects,
an array is made available to each procedure via the upvar
command. The global name of this variable is the procedures internal
name and the local name is GD.
.bp
.ti -4
Defining trigger procedures in PL/Tcl

Trigger procedures are defined in PostgreSQL as functions without
arguments and a return type of opaque. And so are they in the PL/Tcl
language.

The informations from the trigger manager are given to the procedure body
in the following variables:

.in +4
.ti -4
$TG_name
.br
The name of the trigger from the CREATE TRIGGER statement

.ti -4
$TG_relid
.br
The Object ID of the table that caused the trigger procedure to be
called.

.ti -4
$TG_relatts
.br
A Tcl list of the tables field names prefixed with an empty list element.
So looking up an element name in the list with the lsearch Tcl command
returns the same positive number starting from 1 as the fields are numbered
in the pg_attribute system catalog.

.ti -4
$TG_when
.br
The string BEFORE or AFTER, depending on the event of the trigger call.

.ti -4
$TG_level
.br
The string ROW or STATEMENT, depending on the event of the trigger call.

.ti -4
$TG_op
.br
The string INSERT, UPDATE or DELETE, depending on the event of the trigger 
call.

.ti -4
$NEW
.br
An array containing the values of the new table row on INSERT/UPDATE
actions, or empty on DELETE.

.ti -4
$OLD
.br
An array containing the values of the old table row on UPDATE/DELETE
actions, or empty on INSERT.

.ti -4
$GD
.br
The global status data array as described in the functions section of this
document.

.ti -4
$args
.br
A Tcl list of the arguments to the procedure as given in the
CREATE TRIGGER statement. The arguments are also accessible as $1 ... $n
in the procedure body.
.bp
.in -4
The return value from a trigger procedure is one of the strings OK or SKIP,
or a list as returned by the 'array get' Tcl command. If the return value
is OK, the normal operation (INSERT/UPDATE/DELETE) that fired this trigger
will take place. Obviously, SKIP tells the trigger manager to silently
suppress the operation. The list from 'array get' tells PL/Tcl
to return a modified row to the trigger manager that will be inserted instead
of the one given in $NEW (INSERT/UPDATE only). Needless to say that all
this is only meaningful when the trigger is BEFORE and FOR EACH ROW.

Here's a little example trigger procedure that forces an integer value
in a table to keep track of the # of updates that are performed on the
row. For new row's inserted, the value is initialized to 0 and then
incremented on every update operation:

.nf
.in +4
create function trigfunc_modcount() returns opaque as '
    switch $TG_op {
        INSERT {
            set NEW($1) 0
        }
        UPDATE {
            set NEW($1) $OLD($1)
            incr NEW($1)
        }
        default {
            return OK
        }
    }
    return [array get NEW]
.ti -1
 ' language 'pltcl';

create table T1 (key int4, modcnt int4, desc text);

create trigger trig_T1_modcount before insert or update
    on T1 for each row execute procedure
    trigfunc_modcount('modcnt');
.in -4
.fi
.bp
.ti -4
PostgreSQL database access from PL/Tcl

The following commands are available to access the database from
the body of a PL/Tcl procedure:

.in +4
.ti -4
elog level msg
.br
Fire a log message. Possible levels are NOTICE, WARN, ERROR, 
FATAL, and DEBUG
like for the elog() C function.

.ti -4
quote string
.br
Duplicates all occurences of single quote and backslash characters.
It should be used when variables are used in the query string given
to spi_exec or spi_prepare (not for the value list on spi_execp). 
Think about a query string like

.ti +4
select '$val' as ret

where the Tcl variable actually contains "doesn't". This would result
in the final query string 

.ti +4
select 'doesn't' as ret

what's wrong. It should contain

.ti +4
select 'doesn''t'

and should be written as

.ti +4
select '[quote $val]' as ret

to work.

.ti -4
spi_exec ?-count n? ?-array name? query ?loop-body?
.br
Call parser/planner/optimizer/executor for query. 
The optional -count value tells spi_exec the maximum number of rows
to be processed by the query.

If the query is
a SELECT statement and the optional loop-body (a body of Tcl commands
like in a foreach statement) is given, it is evaluated for each 
row selected and behaves like expected on continue/break. The values
of selected fields are put into variables named as the column names. So a

.ti +2
spi_exec "select count(*) as cnt from pg_proc"

will set the variable $cnt to the number of rows in the pg_proc system
catalog. If the option -array is given, the column values are stored
in the associative array named 'name' indexed by the column name
instead of individual variables.

.in +2
.nf
spi_exec -array C "select * from pg_class" {
    elog DEBUG "have table $C(relname)"
}
.fi
.in -2

will print a DEBUG log message for every row of pg_class. The return value
of spi_exec is the number of rows affected by query as found in
the global variable SPI_processed.

.ti -4
spi_prepare query typelist
.br
Prepares AND SAVES a query plan for later execution. It is a bit different
from the C level SPI_prepare in that the plan is automatically copied to the
toplevel memory context. Thus, there is currently no way of preparing a
plan without saving it.

If the query references arguments, the type names must be given as a Tcl
list. The return value from spi_prepare is a query ID to be used in
subsequent calls to spi_execp. See spi_execp for a sample.

.ti -4
spi_execp ?-count n? ?-array name? ?-nulls str? queryid ?values? ?loop-body?

Execute a prepared plan from spi_prepare with variable substitution. 
The optional -count value tells spi_execp the maximum number of rows
to be processed by the query.

The optional value for -nulls is a string of spaces and 'n' characters
telling spi_execp which of the values are NULL's. If given, it must
have exactly the length of the number of values.

The queryid is the ID returned by the spi_prepare call.

If there was a typelist given to spi_prepare, a Tcl list of values of
exactly the same length must be given to spi_execp after the query. If
the type list on spi_prepare was empty, this argument must be omitted.

If the query is a SELECT statement, the same as described for spi_exec
happens for the loop-body and the variables for the fields selected.

Here's an example for a PL/Tcl function using a prepared plan:

.in +4
.nf
create table T1 (key int4, val text);

create function T1_count(int4) returns int4 as '
    if {![info exists GD]} {
        # prepare the plan on the first call
        set GD(plan) [spi_prepare \\\\
          "select count(*) as cnt from T1 where key = \\\\$1" \\\\
          int4]
    }
    spi_execp -count 1 $GD(plan) [list $1]
    return $cnt
.ti -1
 ' language 'pltcl';
.fi
.in -4

Note that each backslash that Tcl should see must be doubled in
the query creating the function, since the PostgreSQL parser processes
backslashes too.
.bp
.ti -4
Modules and the unknown command

PL/Tcl has a special support for things often used. It recognizes two
magic tables, pltcl_modules and pltcl_modfuncs.
If these exist, the module 'unknown' is loaded into the interpreter
right after creation. Whenever an unknown Tcl procedure is called,
the unknown proc is called to check if the procedure is defined in one
of the modules. If this is true, the module is loaded on demand.

See the documentation in the modules subdirectory for detailed
information.



.in -4
Now enjoy PL/Tcl.

jwieck@debis.com (Jan Wieck)
