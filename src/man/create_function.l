.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_function.l,v 1.1 1996/11/14 10:15:48 scrappy Exp $
.TH "CREATE FUNCTION" SQL 11/05/95 Postgres95 Postgres95
.SH "NAME"
create function \(em define a new function
.SH "SYNOPSIS"
.nf
\fBcreate function\fP function_name \fB(\fP
	([type1 {, type-n}])
	\fBreturns\fP type-r
	\fBas\fP {'/full/path/to/objectfile' | 'sql-queries'}
	\fBlanguage\fP {'c' \ 'sql' \ 'internal'}
.fi
.SH "DESCRIPTION"
With this command, a Postgres user can register a function with Postgres.
Subsequently, this user is treated as the owner of the function.
.PP
When defining a function with arguments, the input data types,
.IR type-1 ,
.IR type-2 ,
\&...,
.IR type-n ,
and the return data type,
.IR type-r
must be specified, along with the language, which may be
.IR "\*(lqc\*(rq"
or
.IR "\*(lqsql\*(rq" .
or
.IR "\*(lqinternal\*(rq" .
(The
.IR "arg is"
clause may be left out if the function has no arguments, or
alternatively the argument list may be left empty.)
The input types may be base or complex types, or 
.IR opaque .
.IR Opaque
indicates that the function accepts arguments of an
invalid type such as (char *).
The output type may be specified as a base type, complex type, 
.IR "setof <type>",
or 
.IR opaque .
The 
.IR setof
modifier indicates that the function will return a set of items,
rather than a single item.  
The
.IR as
clause of the command is treated differently for C and SQL
functions, as explained below.
.SH "C FUNCTIONS"
Functions written in C can be defined to Postgres, which will dynamically
load them into its address space.  The loading happens either using
.IR load (l)
or automatically the first time the function is necessary for
execution. Repeated execution of a function will cause negligible
additional overhead, as the function will remain in a main memory
cache.
.PP
Internal functions are functions written in C which have been statically
linked into the postgres backend process.  The 
.BR as
clause must still be specified when defining an internal function but
the contents are ignored.
.SH "Writing C Functions"
The body of a C function following 
.BR as
should be the
.BR "FULL PATH"
of the object code (.o file) for the function, bracketed by quotation
marks.  (Postgres will not compile a function automatically \(em it must
be compiled before it is used in a
.BR "define function"
command.)
.PP
C functions with base type arguments can be written in a
straightforward fashion.  The C equivalents of built-in Postgres types
are accessible in a C file if 
.nf
\&.../src/backend/utils/builtins.h
.fi
is included as a header file.  This can be achieved by having
.nf
\&#include <utils/builtins.h>
.fi
at the top of the C source file and by compiling all C files with the
following include options:
.nf
-I.../src/backend
-I.../src/backend/port/<portname>
-I.../src/backend/obj
.fi
before any \*(lq.c\*(rq programs in the 
.IR cc
command line, e.g.:
.nf
cc -I.../src/backend \e
   -I.../src/backend/port/<portname> \e
   -I.../src/backend/obj \e
   -c progname.c
.fi
where \*(lq...\*(rq is the path to the installed Postgres source tree and
\*(lq<portname>\*(rq is the name of the port for which the source tree
has been built.
.PP
The convention for passing arguments to and from the user's C
functions is to use pass-by-value for data types that are 32 bits (4
bytes) or smaller, and pass-by-reference for data types that require
more than 32 bits.
.if t \{
The following table gives the C type required for parameters in the C
functions that will be loaded into Postgres.  The \*(lqDefined In\*(rq
column gives the actual header file (in the
.nf
\&.../src/backend
.fi
directory) that the equivalent C type is defined.  However, if you
include \*(lqutils/builtins.h\*(rq, these files will automatically be
included.
.SH "Equivalent C Types for Built-In Postgres Types"
.PP
.TS
center;
l l l
l l l.
\fBBuilt-In Type\fP	\fBC Type\fP	\fBDefined In\fP
_
abstime 	AbsoluteTime	utils/nabstime.h
bool	bool	include/c.h
box	(BOX *) 	utils/geo-decls.h
bytea	(bytea *)	include/postgres.h
char	char	N/A
char16	Char16 or (char16 *)	include/postgres.h
cid	CID	include/postgres.h
int2	int2	include/postgres.h
int28	(int28 *)	include/postgres.h
int4	int4	include/postgres.h
float4	float32 or (float4 *)	include/c.h or include/postgres.h
float8	float64 or (float8 *)	include/c.h or include/postgres.h
lseg	(LSEG *)	include/geo-decls.h
name	(Name)	include/postgres.h
oid	oid	include/postgres.h
oid8	(oid8 *)	include/postgres.h
path	(PATH *)	utils/geo-decls.h
point	(POINT *)	utils/geo-decls.h
regproc 	regproc or REGPROC	include/postgres.h
reltime 	RelativeTime 	utils/nabstime.h
text	(text *)	include/postgres.h
tid	ItemPointer	storage/itemptr.h
tinterval	TimeInterval	utils/nabstime.h
uint2	uint16	include/c.h
uint4	uint32	include/c.h
xid	(XID *) 	include/postgres.h
.TE
\}
.PP
Complex arguments to C functions are passed into the C function as a
special C type, TUPLE, defined in
.nf
\&.../src/libpq/libpq-fe.h.
.fi
Given a variable 
.IR t
of this type, the C function may extract attributes from the function
using the function call:
.nf
GetAttributeByName(t, "fieldname", &isnull)
.fi
where 
.IR isnull
is a pointer to a 
.IR bool ,
which the function sets to
.IR true
if the field is null.  The result of this function should be cast
appropriately as shown in the examples below.
.SH "Compiling Dynamically-Loaded C Functions"
.PP
Different operating systems require different procedures for compiling
C source files so that Postgres can load them dynamically.  This section
discusses the required compiler and loader options on each system.
.PP
Under Linux ELF, object files can be generated by specifing the compiler
flag -fpic.
.PP
Under Ultrix, all object files that Postgres is expected to load
dynamically must be compiled using
.IR /bin/cc
with the \*(lq-G 0\*(rq option turned on.  The object file name in the
.IR as
clause should end in \*(lq.o\*(rq.
.PP
Under HP-UX, DEC OSF/1, AIX and SunOS 4, all object files must be
turned into
.IR "shared libraries"
using the operating system's native object file loader,
.IR ld (1).
.PP
Under HP-UX, an object file must be compiled using the native HP-UX C
compiler,
.IR /bin/cc ,
with both the \*(lq+z\*(rq and \*(lq+u\*(rq flags turned on.  The
first flag turns the object file into \*(lqposition-independent
code\*(rq (PIC); the second flag removes some alignment restrictions
that the PA-RISC architecture normally enforces.  The object file must
then be turned into a shared library using the HP-UX loader,
.IR /bin/ld .
The command lines to compile a C source file, \*(lqfoo.c\*(rq, look
like:
.nf
cc <other flags> +z +u -c foo.c
ld <other flags> -b -o foo.sl foo.o
.fi
The object file name in the
.BR as
clause should end in \*(lq.sl\*(rq.
.PP
An extra step is required under versions of HP-UX prior to 9.00.  If
the Postgres header file
.nf
include/c.h
.fi
is not included in the source file, then the following line must also
be added at the top of every source file:
.nf
#pragma HP_ALIGN HPUX_NATURAL_S500
.fi
However, this line must not appear in programs compiled under HP-UX
9.00 or later.
.PP
Under DEC OSF/1, an object file must be compiled and then turned
into a shared library using the OSF/1 loader,
.IR /bin/ld .
In this case, the command lines look like:
.nf
cc <other flags> -c foo.c
ld <other flags> -shared -expect_unresolved '*' -o foo.so foo.o
.fi
The object file name in the
.BR as
clause should end in \*(lq.so\*(rq.
.PP
Under SunOS 4, an object file must be compiled and then turned into a
shared library using the SunOS 4 loader,
.IR /bin/ld .
The command lines look like:
.nf
cc <other flags> -PIC -c foo.c
ld <other flags> -dc -dp -Bdynamic -o foo.so foo.o
.fi
The object file name in the
.BR as
clause should end in \*(lq.so\*(rq.
.PP
Under AIX, object files are compiled normally but building the shared
library requires a couple of steps.  First, create the object file:
.nf
cc <other flags> -c foo.c
.fi
You must then create a symbol \*(lqexports\*(rq file for the object
file:
.nf
mkldexport foo.o `pwd` > foo.exp
.fi
Finally, you can create the shared library:
.nf
ld <other flags> -H512 -T512 -o foo.so -e _nostart \e
   -bI:.../lib/postgres.exp -bE:foo.exp foo.o \e
   -lm -lc 2>/dev/null
.fi
You should look at the Postgres User's Manual for an explanation of this
procedure.
.SH "SQL FUNCTIONS"
SQL functions execute an arbitrary list of SQL queries, returning
the results of the last query in the list.  SQL functions in general
return sets.  If their returntype is not specified as a
.IR setof ,
then an arbitrary element of the last query's result will be returned.
.PP
The body of a SQL function following
.BR as
should be a list of queries separated by whitespace characters and
bracketed within quotation marks.  Note that quotation marks used in
the queries must be escaped, by preceding them with two backslashes
(i.e. \e\e").
.PP
Arguments to the SQL function may be referenced in the queries using
a $n syntax: $1 refers to the first argument, $2 to the second, and so
on.  If an argument is complex, then a \*(lqdot\*(rq notation may be
used to access attributes of the argument (e.g. \*(lq$1.emp\*(rq), or
to invoke functions via a nested-dot syntax.
.SH "EXAMPLES: C Functions"
The following command defines a C function, overpaid, of two basetype
arguments.
.nf
create function overpaid (float8, int4) returns bool
	as '/usr/postgres/src/adt/overpaid.o'
	language 'c'
.fi
The C file "overpaid.c" might look something like:
.nf
#include <utils/builtins.h>

bool overpaid(salary, age)
        float8 *salary;
        int4    age;
{
        if (*salary > 200000.00)
                return(TRUE);
        if ((age < 30) & (*salary > 100000.00))
                return(TRUE);
        return(FALSE);
}
.fi
The overpaid function can be used in a query, e.g:
.nf
select name from EMP where overpaid(salary, age)	
.fi
One can also write this as a function of a single argument of type
EMP:
.nf
create function overpaid_2 (EMP)
	returns bool
	as '/usr/postgres/src/adt/overpaid_2.o'
	language 'c'	
.fi
The following query is now accepted:
.nf
select name from EMP where overpaid_2(EMP)
.fi
In this case, in the body of the overpaid_2 function, the fields in the EMP
record must be extracted.  The C file "overpaid_2.c" might look
something like:
.nf
#include <utils/builtins.h>
#include <libpq-fe.h>

bool overpaid_2(t)
TUPLE t;
{
    float8 *salary;
    int4    age;
    bool    salnull, agenull;

    salary = (float8 *)GetAttributeByName(t, "salary",
                                          &salnull);
    age = (int4)GetAttributeByName(t, "age", &agenull);
    if (!salnull && *salary > 200000.00)
        return(TRUE);
    if (!agenull && (age<30) && (*salary > 100000.00))
        return(TRUE);
    return(FALSE)
}
.fi
.SH "EXAMPLES: SQL Functions"
To illustrate a simple SQL function, consider the following,
which might be used to debit a bank account:
.nf
create function TP1 (int4, float8) returns int4
	as 'update BANK set balance = BANK.balance - $2
		where BANK.acctountno = $1
	    select(x = 1)'
	    language 'sql'
.fi
A user could execute this function to debit account 17 by $100.00 as
follows:
.nf
select (x = TP1( 17,100.0))
.fi
The following more interesting examples take a single argument of type
EMP, and retrieve multiple results:
.nf
select function hobbies (EMP) returns set of HOBBIES
	as 'select (HOBBIES.all) from HOBBIES
		where $1.name = HOBBIES.person'
	language 'sql'
.SH "SEE ALSO"
.PP
information(1), load(l), drop function(l).
.SH "NOTES"
.SH "Name Space Conflicts"
More than one function may be defined with the same name, as long as
the arguments they take are different.  In other words, function names
can be
.IR overloaded .
A function may also have the same name as an attribute.  In the case
that there is an ambiguity between a function on a complex type and
an attribute of the complex type, the attribute will always be used.
.SH "RESTRICTIONS"
The name of the C function must be a legal C function name, and the
name of the function in C code must be exactly the same as the name
used in
.BR "create function" .
There is a subtle implication of this restriction: while the
dynamic loading routines in most operating systems are more than 
happy to allow you to load any number of shared libraries that 
contain conflicting (identically-named) function names, they may 
in fact botch the load in interesting ways.  For example, if you
define a dynamically-loaded function that happens to have the
same name as a function built into Postgres, the DEC OSF/1 dynamic 
loader causes Postgres to call the function within itself rather than 
allowing Postgres to call your function.  Hence, if you want your
function to be used on different architectures, we recommend that 
you do not overload C function names.
.PP
There is a clever trick to get around the problem just described.
Since there is no problem overloading SQL functions, you can 
define a set of C functions with different names and then define 
a set of identically-named SQL function wrappers that take the
appropriate argument types and call the matching C function.
.PP
.IR opaque
cannot be given as an argument to a SQL function.
.SH "BUGS"
C functions cannot return a set of values.
