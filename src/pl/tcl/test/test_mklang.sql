
create function pltcl_call_handler() returns opaque
	as '/usr/local/pgsql/lib/pltcl.so'
	language 'C';

create trusted procedural language 'pltcl'
	handler pltcl_call_handler
	lancompiler 'PL/Tcl';

