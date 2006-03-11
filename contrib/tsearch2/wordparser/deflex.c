/* $PostgreSQL: pgsql/contrib/tsearch2/wordparser/deflex.c,v 1.4 2006/03/11 04:38:30 momjian Exp $ */

#include "deflex.h"

const char *lex_descr[] = {
	"",
	"Latin word",
	"Non-latin word",
	"Word",
	"Email",
	"URL",
	"Host",
	"Scientific notation",
	"VERSION",
	"Part of hyphenated word",
	"Non-latin part of hyphenated word",
	"Latin part of hyphenated word",
	"Space symbols",
	"HTML Tag",
	"Protocol head",
	"Hyphenated word",
	"Latin hyphenated word",
	"Non-latin hyphenated word",
	"URI",
	"File or path name",
	"Decimal notation",
	"Signed integer",
	"Unsigned integer",
	"HTML Entity"
};

const char *tok_alias[] = {
	"",
	"lword",
	"nlword",
	"word",
	"email",
	"url",
	"host",
	"sfloat",
	"version",
	"part_hword",
	"nlpart_hword",
	"lpart_hword",
	"blank",
	"tag",
	"protocol",
	"hword",
	"lhword",
	"nlhword",
	"uri",
	"file",
	"float",
	"int",
	"uint",
	"entity"
};
