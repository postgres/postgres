/*-------------------------------------------------------------------------
 *
 * keywords.c--
 *    lexical token lookup for reserved words in postgres SQL
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/keywords.c,v 1.1.1.1.2.1 1996/08/24 20:54:11 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "parse.h"
#include "utils/elog.h"
#include "parser/keywords.h"
#include "parser/dbcommands.h"		/* createdb, destroydb stop_vacuum */


/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *	 search is used to locate entries.
 */
static ScanKeyword ScanKeywords[] = {
	/* name			value		*/
	{ "abort",		ABORT_TRANS	},
	{ "acl",		ACL		},
	{ "add",		ADD		},
	{ "after",		AFTER		},
	{ "aggregate",          AGGREGATE       },
	{ "all",		ALL		},
	{ "alter",		ALTER		},
	{ "and",		AND		},
	{ "append",		APPEND		},
	{ "archIve",		ARCHIVE		},   /* XXX crooked: I < _ */
	{ "arch_store",		ARCH_STORE	},
	{ "archive",		ARCHIVE		},   /* XXX crooked: i > _ */
        { "as",                 AS              },
        { "asc",		ASC             },
	{ "backward",		BACKWARD	},
	{ "before",		BEFORE		},
	{ "begin",		BEGIN_TRANS	},
	{ "binary",		BINARY		},
	{ "by",			BY		},
	{ "cast",		CAST		},
	{ "change",		CHANGE		},
	{ "close",		CLOSE		},
	{ "cluster",		CLUSTER		},
	{ "column",		COLUMN		},
	{ "commit",		COMMIT		},
	{ "copy",		COPY		},
	{ "create",		CREATE		},
	{ "current",		CURRENT		},
	{ "cursor",		CURSOR		},
	{ "database",		DATABASE	},
	{ "declare",		DECLARE		},
	{ "delete",		DELETE		},
	{ "delimiters",         DELIMITERS      },
	{ "desc",		DESC		},
	{ "distinct",		DISTINCT	},
	{ "do",			DO		},
	{ "drop",		DROP		},
	{ "end",		END_TRANS	},
	{ "execute",		EXECUTE		},
	{ "explain",		EXPLAIN		},
	{ "extend",		EXTEND		},
	{ "fetch",		FETCH		},
	{ "for",		FOR		},
	{ "forward",		FORWARD		},
	{ "from",		FROM		},
	{ "function",		FUNCTION	},
	{ "grant",              GRANT           },
	{ "group",		GROUP		},
	{ "having",		HAVING		},
	{ "heavy",		HEAVY		},
	{ "in",			IN		},
	{ "index",		INDEX		},
	{ "inherits",		INHERITS	},
	{ "insert",		INSERT		},
	{ "instead",		INSTEAD		},
	{ "into",		INTO		},
	{ "isnull",             ISNULL          },
	{ "language",		LANGUAGE	},
	{ "light",		LIGHT		},
	{ "like",		LIKE		},
	{ "listen",             LISTEN          },
	{ "load",               LOAD            },
	{ "merge",		MERGE		},
	{ "move",		MOVE		},
	{ "new",		NEW		},
	{ "none",		NONE		},
	{ "not",		NOT		},
	{ "nothing",            NOTHING         },
	{ "notify",             NOTIFY          },
	{ "notnull",            NOTNULL         },
	{ "null",		PNULL		},
	{ "oids",		OIDS		},
	{ "on",			ON		},
	{ "operator",		OPERATOR	},
        { "option",             OPTION          },
	{ "or",			OR		},
	{ "order",		ORDER		},
        { "privileges",         PRIVILEGES      },
	{ "public",             PUBLIC          },
	{ "purge",		PURGE		},
	{ "recipe",             RECIPE          },
	{ "rename",		RENAME		},
	{ "replace",		REPLACE		},
	{ "retrieve",		RETRIEVE	},
	{ "returns",		RETURNS		},
        { "revoke",             REVOKE          },
	{ "rollback",		ROLLBACK	},
	{ "rule",		RULE		},
	{ "select",		SELECT		},
	{ "set",		SET		},
	{ "setof",		SETOF		},
	{ "stdin",		STDIN		},
	{ "stdout",		STDOUT		},
	{ "store",		STORE		},
	{ "table",		TABLE		},
	{ "to",			TO		},
	{ "transaction",	TRANSACTION	},
	{ "type",		P_TYPE		},
	{ "update",		UPDATE		},
	{ "using",		USING		},
	{ "vacuum",		VACUUM		},
	{ "values",		VALUES		},
	{ "version",		VERSION		},
	{ "view",		VIEW		},
	{ "where",		WHERE		},
	{ "with",		WITH		},
	{ "work",		WORK		},
};

ScanKeyword *
ScanKeywordLookup(char *text)
{
    ScanKeyword	*low	= &ScanKeywords[0];
    ScanKeyword	*high	= endof(ScanKeywords) - 1;
    ScanKeyword	*middle;
    int		difference;
    
    while (low <= high) {
	middle = low + (high - low) / 2;
	/* keywords case-insensitive (for SQL) -- ay 8/94 */
	difference = strcasecmp(middle->name, text);	
	if (difference == 0)
	    return (middle);
	else if (difference < 0)
	    low = middle + 1;
	else
	    high = middle - 1;
    }
    
    return (NULL);
}

char*
AtomValueGetString(int atomval)
{
    ScanKeyword *low = &ScanKeywords[0];
    ScanKeyword *high = endof(ScanKeywords) - 1;
    int keyword_list_length = (high-low);
    int i;
    
    for (i=0; i < keyword_list_length  ; i++ )
	if (ScanKeywords[i].value == atomval )
	    return(ScanKeywords[i].name);
    
    elog(WARN,"AtomGetString called with bogus atom # : %d", atomval );
    return(NULL);
}
