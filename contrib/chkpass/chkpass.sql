--
--	PostgreSQL code for CHKPASS.
--  Written by D'Arcy J.M. Cain
--  darcy@druid.net
--  http://www.druid.net/darcy/
-- 
--  $Header: /cvsroot/pgsql/contrib/chkpass/Attic/chkpass.sql,v 1.1 2001/05/03 12:32:13 darcy Exp $
--  best viewed with tabs set to 4
--  %%PGDIR%% changed to your local directory where modules is
--

load '%%PGDIR%%/modules/chkpass.so';

--
--	Input and output functions and the type itself:
--

create function chkpass_in(opaque)
	returns opaque
	as '%%PGDIR%%/modules/chkpass.so'
	language 'c';

create function chkpass_out(opaque)
	returns opaque
	as '%%PGDIR%%/modules/chkpass.so'
	language 'c';

create type chkpass (
	internallength = 16,
	externallength = 13,
	input = chkpass_in,
	output = chkpass_out
);

create function raw(chkpass)
	returns text
	as '%%PGDIR%%/modules/chkpass.so', 'chkpass_rout'
	language 'c';

--
--	The various boolean tests:
--

create function eq(chkpass, text)
	returns bool
	as '%%PGDIR%%/modules/chkpass.so', 'chkpass_eq'
	language 'c';

create function ne(chkpass, text)
	returns bool
	as '%%PGDIR%%/modules/chkpass.so', 'chkpass_ne'
	language 'c';

--
--	Now the operators.  Note how some of the parameters to some
--	of the 'create operator' commands are commented out.  This
--	is because they reference as yet undefined operators, and
--	will be implicitly defined when those are, further down.
--

create operator = (
	leftarg = chkpass,
	rightarg = text,
	commutator = =,
--	negator = <>,
	procedure = eq
);

create operator <> (
	leftarg = chkpass,
	rightarg = text,
	negator = =,
	procedure = ne
);

INSERT INTO pg_description (objoid, description)
	SELECT oid, 'password type with checks'
		FROM pg_type WHERE typname = 'chkpass';

--
--	eof
--
