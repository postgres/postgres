--
--	PostgreSQL code for MAC addresses.
--
--	$Id: mac.sql,v 1.2 1998/02/14 17:58:08 scrappy Exp $
--

load '/usr/local/pgsql/modules/mac.so';

--
--	Input and output functions and the type itself:
--

create function macaddr_in(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_out(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create type macaddr (
	internallength = 6,
	externallength = variable,
	input = macaddr_in,
	output = macaddr_out
);

--
--	The boolean tests:
--

create function macaddr_lt(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_le(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_eq(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_ge(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_gt(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_ne(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

--
--	Now the operators.  Note how some of the parameters to some
--	of the 'create operator' commands are commented out.  This
--	is because they reference as yet undefined operators, and
--	will be implicitly defined when those are, further down.
--

create operator < (
	leftarg = macaddr,
	rightarg = macaddr,
--	negator = >=,
	procedure = macaddr_lt
);

create operator <= (
	leftarg = macaddr,
	rightarg = macaddr,
--	negator = >,
	procedure = macaddr_le
);

create operator = (
	leftarg = macaddr,
	rightarg = macaddr,
	commutator = =,
--	negator = <>,
	procedure = macaddr_eq
);

create operator >= (
	leftarg = macaddr,
	rightarg = macaddr,
	negator = <,
	procedure = macaddr_ge
);

create operator > (
	leftarg = macaddr,
	rightarg = macaddr,
	negator = <=,
	procedure = macaddr_gt
);

create operator <> (
	leftarg = macaddr,
	rightarg = macaddr,
	negator = =,
	procedure = macaddr_ne
);

--
--	Finally, the special manufacurer matching function:
--

create function macaddr_manuf(macaddr)
	returns text
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

--
--	eof
--
