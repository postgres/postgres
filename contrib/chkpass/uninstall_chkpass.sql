SET search_path = public;

DROP OPERATOR <>; (
	leftarg = chkpass,
	rightarg = text,
	negator = =,
	procedure = ne
);

DROP OPERATOR =; (
	leftarg = chkpass,
	rightarg = text,
	commutator = =,
--	negator = <>,
	procedure = eq
);

DROP FUNCTION ne(chkpass, text);

DROP FUNCTION eq(chkpass, text);

DROP FUNCTION raw(chkpass);

DROP TYPE chkpass;

DROP FUNCTION chkpass_out(chkpass);

DROP FUNCTION chkpass_in(cstring);
