--
-- PL/pgSQL language declaration
--
-- $Header: /cvsroot/pgsql/src/pl/plpgsql/src/Attic/mklang.sql,v 1.1 1998/08/24 19:14:48 momjian Exp $
--

create function plpgsql_call_handler() returns opaque
	as '/usr/local/pgsql/lib/plpgsql.so'
	language 'C';

create trusted procedural language 'plpgsql'
	handler plpgsql_call_handler
	lancompiler 'PL/pgSQL';

