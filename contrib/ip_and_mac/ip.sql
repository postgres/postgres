--
--	PostgreSQL code for IP addresses.
--
--	$Id: ip.sql,v 1.2 1998/02/14 17:58:04 scrappy Exp $
--

load '/usr/local/pgsql/modules/ip.so';

--
--	Input and output functions and the type itself:
--

create function ipaddr_in(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_out(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create type ipaddr (
	internallength = 6,
	externallength = variable,
	input = ipaddr_in,
	output = ipaddr_out
);

--
--	The various boolean tests:
--

create function ipaddr_lt(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_le(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_eq(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_ge(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_gt(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_ne(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_in_net(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_mask(ipaddr)
	returns ipaddr
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

create function ipaddr_bcast(ipaddr)
	returns ipaddr
	as '/usr/local/pgsql/modules/ip.so'
	language 'c';

--
--	Now the operators.  Note how some of the parameters to some
--	of the 'create operator' commands are commented out.  This
--	is because they reference as yet undefined operators, and
--	will be implicitly defined when those are, further down.
--

create operator < (
	leftarg = ipaddr,
	rightarg = ipaddr,
--	negator = >=,
	procedure = ipaddr_lt
);

create operator <= (
	leftarg = ipaddr,
	rightarg = ipaddr,
--	negator = >,
	procedure = ipaddr_le
);

create operator = (
	leftarg = ipaddr,
	rightarg = ipaddr,
	commutator = =,
--	negator = <>,
	procedure = ipaddr_eq
);

create operator >= (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = <,
	procedure = ipaddr_ge
);

create operator > (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = <=,
	procedure = ipaddr_gt
);

create operator <> (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = =,
	procedure = ipaddr_ne
);

--
--	eof
--
