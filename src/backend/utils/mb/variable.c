/*
 * This file contains some public functions
 * related to show/set/reset variable commands.
 * Tatsuo Ishii
 * $Id: variable.c,v 1.1 1998/07/24 03:31:57 scrappy Exp $
 */

#include "mb/pg_wchar.h"

bool
parse_client_encoding(const char *value)
{
  int encoding;

  encoding = pg_valid_client_encoding(value);
  if (encoding < 0) {
    elog(ERROR, "Client encoding %s is not supported", value);
  } else {    
    if (pg_set_client_encoding(encoding)) {
      elog(ERROR, "Conversion between %s and %s is not supported",
	   value, pg_encoding_to_char(GetDatabaseEncoding()));
    }
  }
  return TRUE;
}

bool
show_client_encoding()
{
  elog(NOTICE, "Current client encoding is %s",
       pg_encoding_to_char(pg_get_client_encoding()));
  return TRUE;
}

bool
reset_client_encoding()
{
  int encoding;
  char *env = getenv("PGCLIENTENCODING");

  if (env) {
    encoding = pg_char_to_encoding(env);
    if (encoding < 0) {
      encoding = GetDatabaseEncoding();
    }
  } else {
    encoding = GetDatabaseEncoding();
  }
  pg_set_client_encoding(encoding);
  return TRUE;
}

bool
parse_server_encoding(const char *value)
{
  elog(NOTICE, "SET SERVER_ENCODING is not supported");
  return TRUE;
}

bool
show_server_encoding()
{
  elog(NOTICE, "Current server encoding is %s",
       pg_encoding_to_char(GetDatabaseEncoding()));
  return TRUE;
}

bool
reset_server_encoding()
{
  elog(NOTICE, "RESET SERVER_ENCODING is not supported");
  return TRUE;
}
