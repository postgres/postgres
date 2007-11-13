/* $PostgreSQL: pgsql/contrib/dict_int/uninstall_dict_int.sql,v 1.3 2007/11/13 04:24:27 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP TEXT SEARCH DICTIONARY intdict;

DROP TEXT SEARCH TEMPLATE intdict_template;

DROP FUNCTION dintdict_init(internal);

DROP FUNCTION dintdict_lexize(internal,internal,internal,internal);
