--
--	PostgreSQL code for IP addresses.
--
--	$Id: ip.sql,v 1.5 1998/06/16 05:35:10 momjian Exp $
--      Invoced from  1998/02/14 17:58:04 scrappy
--
--      New - INPUT/OUTPUT, functions, indexing by btree, test.
--      PART # 1 - ip.sql - load new type, functions and operators.
--      Then you should execute ipi.sql - add ipaddr_ops class to allow indexing.

load '/usr/local/pgsql/contrib/ip_and_macs/ip.so';

--
--	Input and output functions and the type itself:
--      Note - we input 193.124.23.1 as /32, and 193.124.23.0 as /24.
--      We output /24 network withouth /24 suffix, and /32 hosts wothouth suffixes
--      if it is not '0' address of /24 network.
--      Just the same, we threat 0.0.0.0 as 0.0.0.0/0 == DEFAULT.
--

create function ipaddr_in(opaque)
	returns opaque
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';



create function ipaddr_out(opaque)
	returns opaque
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create type ipaddr (
	internallength = 6,
	externallength = variable,
	input = ipaddr_in,
	output = ipaddr_out
);

--
-- Print address by format
-- %A - address
-- %P - /Pref
-- %M - maska
-- %B - reversed maska
drop function ipaddr_print;
create function ipaddr_print(ipaddr, text)
	returns text
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';
);

--
--	The various boolean tests:
--      In case if addresseas are equal, we compare prefix length
--      It means 193.124.23.0/24 < 193.124.23.0/32
--

create function ipaddr_lt(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create function ipaddr_le(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create function ipaddr_eq(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create function ipaddr_ge(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create function ipaddr_gt(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

create function ipaddr_ne(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Test if a1 is in net a2
-- Return TRUE if a1 is IN a2 subnet or if a1 == a2
--
create function ipaddr_in_net(ipaddr, ipaddr)
	returns bool
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return the network from the host/network address. This means
-- 193.124.23.4/24 -> 193.124.23.0/24.
-- This allow to use interface address (with the real netmask) to create
-- network, and to link interfaces and addresses belongs to the same network.
--

 create function ipaddr_net(ipaddr)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return TRUE if addr describe NETWORK, not host in the network
-- It's equivalent to ipaddr_net(a) == a
--

 create function ipaddr_is_net(ipaddr)
	returns boolean
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return the number of the hosts in the network
--

 create function ipaddr_len(ipaddr)
	returns int4
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return the prefix length of the network
--

 create function ipaddr_pref(ipaddr)
	returns int4
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Convert network into the integer.
-- Can be used for 'compose' function
--

 create function ipaddr_integer(ipaddr)
	returns int4
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Compose ipaddr from the ADDRESS and PREF
-- ipaddr_compose(ipaddr_integer(a),ipaddr_pref(a)) == a
--

 create function ipaddr_compose(int4,int4)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return MASK for the network
--

 create function ipaddr_mask(ipaddr)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Return BROADCAST address for the network
--

 create function ipaddr_bcast(ipaddr)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Compare 2 addresses. First, compare addresses, then, compare prefixes (if the addresses
-- are the same).
--

 create function ipaddr_cmp(ipaddr,ipaddr)
	returns int4
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
-- Plus and Minus operators
--

 create function ipaddr_plus(ipaddr,int4)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

 create function ipaddr_minus(ipaddr,int4)
	returns ipaddr
	as '/usr/local/pgsql/contrib/ip_and_macs/ip.so'
	language 'c';

--
--	Now the operators.  Note how some of the parameters to some
--	of the 'create operator' commands are commented out.  This
--	is because they reference as yet undefined operators, and
--	will be implicitly defined when those are, further down.
--

-- drop operator < ( ipaddr, ipaddr);
create operator < (
	leftarg = ipaddr,
	rightarg = ipaddr,
--	negator = >=,
	procedure = ipaddr_lt,
	restrict = intltsel,
	join = intltjoinsel
);

-- drop operator <= (ipaddr,ipaddr);
create operator <= (
	leftarg = ipaddr,
	rightarg = ipaddr,
--	negator = >,
	procedure = ipaddr_le,
	restrict = intltsel,
	join = intltjoinsel
);

-- drop operator = (ipaddr,ipaddr);
create operator = (
	leftarg = ipaddr,
	rightarg = ipaddr,
	commutator = =,
--	negator = <>,
	restrict = eqsel,
	join = eqjoinsel,
	procedure = ipaddr_eq
);


-- drop operator >= (ipaddr,ipaddr);
create operator >= (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = <,
	procedure = ipaddr_ge,
	restrict = intgtsel,
	join = intgtjoinsel
);

-- drop operator > (ipaddr,ipaddr);
create operator > (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = <=,
	procedure = ipaddr_gt,
	restrict = intgtsel,
	join = intgtjoinsel
);

-- drop operator <> (ipaddr,ipaddr);
create operator <> (
	leftarg = ipaddr,
	rightarg = ipaddr,
	negator = =,
	procedure = ipaddr_ne,
	restrict = neqsel,
	join = neqjoinsel
);

create operator @ (
	leftarg = ipaddr,
	rightarg = ipaddr,
	procedure = ipaddr_in_net
);

create operator + (
	leftarg = ipaddr,
	rightarg = int4,
	procedure = ipaddr_plus
);

create operator - (
	leftarg = ipaddr,
	rightarg = int4,
	procedure = ipaddr_minus
);

-- *****************************************************************************************
-- * For now, you have: input/output (remember, '193.124.23.0' means /24 network,          *
-- *                                            '193.124.23.1' means /32 host)             *
-- * <, <=, = <>, >=, > relational operations; host @ net (host is the part of the net) op *
-- * varchar ipaddr_print(addr, '%A/%P %M %B') - print by pattern function                 *
-- * ipaddr ipaddr_mask(a),ipaddr_bcast(a),ipaddr_net(a) functions (mask,bcast, start addr)*
-- * int4 ipaddr_len(a) - lenght of subnet; ipaddr_pref(a) - prefix length,                *
-- * int4 ipaddr_integer(a) - integer value; ipaddr ipaddr_compose(integer_addr,pref_len)  *
-- *                                                compose ipaddr from addr and mask      *
-- * '+' and '-' operators (ipaddr = ipaddr + integer),(ipaddr = ipaddr - integer) ops     *
-- *****************************************************************************************
-- *   R E A D    T H I S    T E X T   B E F O R E   E X I T I N G :                       *
-- *       Now you should execute ipi.sql to allow BTREE indexing on this class.           *
-- *****************************************************************************************
-- eof
