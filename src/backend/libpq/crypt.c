/*-------------------------------------------------------------------------
 *
 * crypt.c--
 *        Look into pg_user and check the encrypted password with the one
 *        passed in from the frontend.
 *
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include <postgres.h>
#include <libpq/crypt.h>
#include <utils/nabstime.h>

char* crypt_getpwdfilename() {

  static char*     filename = NULL;

  if (!filename) {
    char*     env;

    env = getenv("PGDATA");
    filename = (char*)malloc(strlen(env) + strlen(CRYPT_PWD_FILE) + 2);
    sprintf(filename, "%s/%s", env, CRYPT_PWD_FILE);
  }

  return filename;
}

/*-------------------------------------------------------------------------*/

static
FILE* crypt_openpwdfile() {

  char*     filename;

  filename = crypt_getpwdfilename();
  return (fopen(filename, "r"));
}

/*-------------------------------------------------------------------------*/

static
void crypt_parsepwdfile(FILE* datafile, char** login, char** pwd, char** valdate) {

  char     buffer[256];
  char*    parse;
  int      count,
           i;

  fgets(buffer, 256, datafile);
  parse = buffer;

  /* store a copy of user login to return
   */
  count = strcspn(parse, "#");
  *login = (char*)malloc(count + 1);
  strncpy(*login, parse, count);
  (*login)[count] = '\0';
  parse += (count + 1);

  /* skip to the password field
   */
  for (i = 0; i < 5; i++)
    parse += (strcspn(parse, "#") + 1);

  /* store a copy of user password to return
   */
  count = strcspn(parse, "#");
  *pwd = (char*)malloc(count + 1);
  strncpy(*pwd, parse, count);
  (*pwd)[count] = '\0';
  parse += (count + 1);

  /* store a copy of date login becomes invalid
   */
  count = strcspn(parse, "#");
  *valdate = (char*)malloc(count + 1);
  strncpy(*valdate, parse, count);
  (*valdate)[count] = '\0';
  parse += (count + 1);
}

/*-------------------------------------------------------------------------*/

static
void crypt_getloginfo(const char* user, char** passwd, char** valuntil) {

  FILE*     datafile;
  char*     login;
  char*     pwd;
  char*     valdate;

  *passwd = NULL;
  *valuntil = NULL;

  if (!(datafile = crypt_openpwdfile()))
    return;

  while (!feof(datafile)) {
    crypt_parsepwdfile(datafile, &login, &pwd, &valdate);
    if (!strcmp(login, user)) {
      free((void*)login);
      *passwd = pwd;
      *valuntil = valdate;
      fclose(datafile);
      return;
    }
    free((void*)login);
    free((void*)pwd);
    free((void*)valdate);
  }
  fclose(datafile);
}

/*-------------------------------------------------------------------------*/

MsgType crypt_salt(const char* user) {

  char*     passwd;
  char*     valuntil;

  crypt_getloginfo(user, &passwd, &valuntil);

  if (passwd == NULL || *passwd == '\0') {
    if (passwd) free((void*)passwd);
    if (valuntil) free((void*)valuntil);
    return STARTUP_UNSALT_MSG;
  }

  free((void*)passwd);
  if (valuntil) free((void*)valuntil);
  return STARTUP_SALT_MSG;
}

/*-------------------------------------------------------------------------*/

int crypt_verify(Port* port, const char* user, const char* pgpass) {

  char*            passwd;
  char*            valuntil;
  char*            crypt_pwd;
  int              retval = STATUS_ERROR;
  AbsoluteTime     vuntil,
                   current;

  crypt_getloginfo(user, &passwd, &valuntil);

  if (passwd == NULL || *passwd == '\0') {
    if (passwd) free((void*)passwd);
    if (valuntil) free((void*)valuntil);
    return STATUS_ERROR;
  }

  crypt_pwd = crypt(passwd, port->salt);
  if (!strcmp(pgpass, crypt_pwd)) {
    /* check here to be sure we are not past valuntil
     */
    if (!valuntil)
      vuntil = INVALID_ABSTIME;
    else
      vuntil = nabstimein(valuntil);
    current = GetCurrentAbsoluteTime();
    if (vuntil != INVALID_ABSTIME && vuntil < current)
      retval = STATUS_ERROR;
    else
      retval = STATUS_OK;
  }

  free((void*)passwd);
  if (valuntil) free((void*)valuntil);
  
  return retval;
}
