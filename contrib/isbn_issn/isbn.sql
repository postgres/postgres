--
--	PostgreSQL code for ISBNs.
--
--	$Id: isbn.sql,v 1.1 1998/08/17 03:35:05 scrappy Exp $
--

load '/usr/local/pgsql/modules/isbn.so';

--
--	Input and output functions and the type itself:
--

create function isbn_in(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_out(opaque)
	returns opaque
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create type isbn (
	internallength = 16,
	externallength = 13,
	input = isbn_in,
	output = isbn_out
);

--
--	The various boolean tests:
--

create function isbn_lt(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_le(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_eq(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_ge(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_gt(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

create function isbn_ne(isbn, isbn)
	returns bool
	as '/usr/local/pgsql/modules/isbn.so'
	language 'c';

--
--	Now the operators.  Note how some of the parameters to some
--	of the 'create operator' commands are commented out.  This
--	is because they reference as yet undefined operators, and
--	will be implicitly defined when those are, further down.
--

create operator < (
	leftarg = isbn,
	rightarg = isbn,
--	negator = >=,
	procedure = isbn_lt
);

create operator <= (
	leftarg = isbn,
	rightarg = isbn,
--	negator = >,
	procedure = isbn_le
);

create operator = (
	leftarg = isbn,
	rightarg = isbn,
	commutator = =,
--	negator = <>,
	procedure = isbn_eq
);

create operator >= (
	leftarg = isbn,
	rightarg = isbn,
	negator = <,
	procedure = isbn_ge
);

create operator > (
	leftarg = isbn,
	rightarg = isbn,
	negator = <=,
	procedure = isbn_gt
);

create operator <> (
	leftarg = isbn,
	rightarg = isbn,
	negator = =,
	procedure = isbn_ne
);

--
--	eof
--
