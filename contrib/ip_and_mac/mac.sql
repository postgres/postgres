--
--	PostgreSQL code for MAC addresses.
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
	internallength = 8,
	externallength = variable,
	input = macaddr_in,
	output = macaddr_out
);

--
--	The various boolean tests:
--

create function macaddr_eq(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_ne(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

create function macaddr_like(macaddr, macaddr)
	returns bool
	as '/usr/local/pgsql/modules/mac.so'
	language 'c';

--
--	Now the operators.  Note how the "negator = <>" in the
--	definition of the equivalence operator is commented out.
--	It gets defined implicitly when "<>" is defined, with
--	"=" as its negator.
--

create operator = (
	leftarg = macaddr,
	rightarg = macaddr,
	commutator = =,
--	negator = <>,
	procedure = macaddr_eq
);

create operator <> (
	leftarg = macaddr,
	rightarg = macaddr,
	commutator = <>,
	negator = =,
	procedure = macaddr_ne
);

create operator ~~ (
	leftarg = macaddr,
	rightarg = macaddr,
	commutator = ~~,
	procedure = macaddr_like
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
