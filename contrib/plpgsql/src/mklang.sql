--
-- PL/pgSQL language declaration
--
-- $Header: /cvsroot/pgsql/contrib/plpgsql/src/Attic/mklang.sql,v 1.1 1998/08/22 12:38:31 momjian Exp $
--

create function plpgsql_call_handler() returns opaque
	as '/usr/local/pgsql/lib/plpgsql.so'
	language 'C';

create trusted procedural language 'plpgsql'
	handler plpgsql_call_handler
	lancompiler 'PL/pgSQL';

