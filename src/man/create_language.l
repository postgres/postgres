.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_language.l,v 1.2 1998/01/11 22:17:14 momjian Exp $
.TH "CREATE LANGUAGE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH "NAME"
create language - define a new language for functions
.SH "SYNOPSIS"
.nf
\fBcreate\fP [\fBtrusted\fP] \fBprocedural language\fP 'lanname'
	\fBhandler\fP call_handler
	\fBlancompiler\fP 'comment'
.fi
.SH "DESCRIPTION"
With this command, a Postgres user can register a new language with
Postgres. Subsequently, functions and trigger procedures can be 
defined in this new language. The user must have the Postgres superuser
privilege to register a new language.
.PP
The lanname is the name of the new procedural language. It is converted
to lower case before the new entry in the pg_language system catalog
is inserted. Note that this case translation is also done on
create function(l) and drop language(l). Thus, the language name
is case insensitive. A procedural language cannot override one of the
builtin languages of Postgres.
.PP
The argument for \fBhandler\fP
is the name of a previously registered function that
will be called to execute the PL procedures.
.PP
The \fBlancompiler\fP argument is the string that will be inserted
in the lancompiler attribute of the new pg_language entry. Up to now,
Postgres doesn't use this attribute in any way. 
.PP
The \fBtrusted\fP keyword specifies, that the call handler for the
language is safe - i.e. it offers an unprivileged user no functionality
to get around access restrictions. If this keyword is omitted when
registering the language, only users with the Postgres superuser privilege
can use this language to create new functions (like the 'C' language).
.SH "WRITING PL HANDLERS"
The call handler for a procedural language must be written in a compiler
language such as 'C' and registered with Postgres as a function taking
no arguments and returning
.IR "opaque"
type. This prevents the call handler from beeing called directly as a function
from queries.
But there are arguments
on the actual call when a PL function or trigger procedure in the
language offered by the handler is to be executed.
.PP
When called from the trigger manager, the only argument is the object ID from
the procedures pg_proc entry. All other information from the trigger manager
is found in the global CurrentTriggerData pointer.
.PP
When called from the function manager, the arguments are the object ID of the
procedures pg_proc entry, the number of arguments given to the PL function,
the arguments in a FmgrValues structure and a pointer to a boolean where the
function tells the caller if the return value is the SQL NULL value.
.PP
It's up to the call handler to fetch the pg_proc entry
and to analyze the argument and return types of the called procedure.
the
.IR "as"
clause from the create function(l) of the procedure will be found in
the prosrc attribute of the pg_proc entry. This may be the source text
in the procedural language itself (like for PL/Tcl), a pathname to a
file or anything else that tells the call handler what to do in detail.
.SH "EXAMPLE"
Following is a template for a PL handler written in 'C':
.nf

#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "fmgr.h"		/* for FmgrValues struct */
#include "access/heapam.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"

Datum
plsample_call_handler(
	Oid		prooid,
	int		pronargs,
	FmgrValues	*proargs,
	bool		*isNull)
{
	Datum		retval;
	TriggerData	*trigdata;

	if (CurrentTriggerData == NULL) {
		/*
		 * Called as a function
		 */

		retval = ...
	} else {
		/*
		 * Called as a trigger procedure
		 */
		trigdata = CurrentTriggerData;
		CurrentTriggerData = NULL;

		retval = ...
	}

	*isNull = false;
	return retval;
}

.fi
Only a few thousand lines of code have to be added instead of the dots
to complete the PL call handler. See create function(l) how to compile
it into a loadable module. The following commands then register the
sample procedural language.
.nf

create function plsample_call_handler () returns opaque
	as '/usr/local/pgsql/lib/plsample.so'
	language 'C';

create procedural language 'plsample'
	handler plsample_call_handler
	lancompiler 'PL/Sample';

.fi
.SH "SEE ALSO"
.PP
create function(l), drop language(l).
.SH "RESTRICTIONS"
Since the call handler for a procedural language must be
registered with Postgres in the 'C' language, it inherits
all the restrictions of 'C' functions.
.SH "BUGS"
Currently, the definitions for a procedural language once
created cannot be changed. 
