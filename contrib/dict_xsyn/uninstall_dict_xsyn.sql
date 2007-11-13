/* $PostgreSQL: pgsql/contrib/dict_xsyn/uninstall_dict_xsyn.sql,v 1.3 2007/11/13 04:24:27 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP TEXT SEARCH DICTIONARY xsyn;

DROP TEXT SEARCH TEMPLATE xsyn_template;

DROP FUNCTION dxsyn_init(internal);

DROP FUNCTION dxsyn_lexize(internal,internal,internal,internal);
