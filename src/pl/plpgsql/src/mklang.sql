--
-- PL/pgSQL language declaration
--
-- $Header: /cvsroot/pgsql/src/pl/plpgsql/src/Attic/mklang.sql,v 1.2 1998/10/12 04:32:24 momjian Exp $
--

create function plpgsql_call_handler() returns opaque
	as '${exec_prefix}/lib/plpgsql.so'
	language 'C';

create trusted procedural language 'plpgsql'
	handler plpgsql_call_handler
	lancompiler 'PL/pgSQL';

