/* $PostgreSQL: pgsql/contrib/chkpass/uninstall_chkpass.sql,v 1.5 2007/11/13 04:24:27 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP OPERATOR <>(chkpass, text);

DROP OPERATOR =(chkpass, text);

DROP FUNCTION ne(chkpass, text);

DROP FUNCTION eq(chkpass, text);

DROP FUNCTION raw(chkpass);

DROP TYPE chkpass CASCADE;
