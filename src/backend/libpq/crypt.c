/*-------------------------------------------------------------------------
 *
 * crypt.c--
 *        Look into pg_user and check the encrypted password with the one
 *        passed in from the frontend.
 *
 * Modification History
 *
 * Dec 17, 1997 - Todd A. Brandys
 *	Orignal Version Completed.
 *
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"
#include "utils/nabstime.h"
#include "storage/fd.h"
#include "libpq/crypt.h"

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

char**     pwd_cache = NULL;
int        pwd_cache_count = 0;

/*-------------------------------------------------------------------------*/

char* crypt_getpwdfilename() {

  static char*     pfnam = NULL;

  if (!pfnam) {
    pfnam = (char*)malloc(strlen(DataDir) + strlen(CRYPT_PWD_FILE) + 2);
    sprintf(pfnam, "%s/%s", DataDir, CRYPT_PWD_FILE);
  }

  return pfnam;
}

/*-------------------------------------------------------------------------*/

char* crypt_getpwdreloadfilename() {

  static char*     rpfnam = NULL;

  if (!rpfnam) {
    char*     pwdfilename;

    pwdfilename = crypt_getpwdfilename();
    rpfnam = (char*)malloc(strlen(pwdfilename) + strlen(CRYPT_PWD_RELOAD_SUFX) + 1);
    sprintf(rpfnam, "%s%s", pwdfilename, CRYPT_PWD_RELOAD_SUFX);
  }

  return rpfnam;
}

/*-------------------------------------------------------------------------*/

static
FILE* crypt_openpwdfile() {
  char*     filename;
  FILE*     pwdfile;

  filename = crypt_getpwdfilename();
  pwdfile = AllocateFile(filename, "r");

  return pwdfile;
}

/*-------------------------------------------------------------------------*/

static
int compar_user(const void* user_a, const void* user_b) {

  int     min,
          value;
  char*   login_a;
  char*   login_b;

  login_a = *((char**)user_a);
  login_b = *((char**)user_b);

  /* We only really want to compare the user logins which are first.  We look
   * for the first SEPSTR char getting the number of chars there are before it.
   * We only need to compare to the min count from the two strings.
   */
  min = strcspn(login_a, CRYPT_PWD_FILE_SEPSTR);
  value = strcspn(login_b, CRYPT_PWD_FILE_SEPSTR);
  if (value < min)
    min = value;

  /* We add one to min so that the separator character is included in the
   * comparison.  Why?  I believe this will prevent logins that are proper
   * prefixes of other logins from being 'masked out'.  Being conservative!
   */
  return strncmp(login_a, login_b, min + 1);
}

/*-------------------------------------------------------------------------*/

static
void crypt_loadpwdfile() {

  char*     filename;
  int       result;
  FILE*     pwd_file;
  char      buffer[256];

  filename = crypt_getpwdreloadfilename();
  result = unlink(filename);

  /* We want to delete the flag file before reading the contents of the pg_pwd
   * file.  If result == 0 then the unlink of the reload file was successful.
   * This means that a backend performed a COPY of the pg_user file to
   * pg_pwd.  Therefore we must now do a reload.
   */
  if (!pwd_cache || !result) {
    if (pwd_cache) {	/* free the old data only if this is a reload */
      while (pwd_cache_count--) {
        free((void*)pwd_cache[pwd_cache_count]);
      }
      free((void*)pwd_cache);
      pwd_cache = NULL;
      pwd_cache_count = 0;
    }

    if (!(pwd_file = crypt_openpwdfile()))
      return;

    /* Here is where we load the data from pg_pwd.
     */
    while (fgets(buffer, 256, pwd_file) != NULL) {
      /* We must remove the return char at the end of the string, as this will
       * affect the correct parsing of the password entry.
       */
      if (buffer[(result = strlen(buffer) - 1)] == '\n')
        buffer[result] = '\0';

      pwd_cache = (char**)realloc((void*)pwd_cache, sizeof(char*) * (pwd_cache_count + 1));
      pwd_cache[pwd_cache_count++] = strdup(buffer);
    }
    fclose(pwd_file);

    /* Now sort the entries in the cache for faster searching later.
     */
    qsort((void*)pwd_cache, pwd_cache_count, sizeof(char*), compar_user);
  }
}

/*-------------------------------------------------------------------------*/

static
void crypt_parsepwdentry(char* buffer, char** pwd, char** valdate) {

  char*    parse = buffer;
  int      count,
           i;

  /* skip to the password field
   */
  for (i = 0; i < 6; i++)
    parse += (strcspn(parse, CRYPT_PWD_FILE_SEPSTR) + 1);

  /* store a copy of user password to return
   */
  count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
  *pwd = (char*)malloc(count + 1);
  strncpy(*pwd, parse, count);
  (*pwd)[count] = '\0';
  parse += (count + 1);

  /* store a copy of date login becomes invalid
   */
  count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
  *valdate = (char*)malloc(count + 1);
  strncpy(*valdate, parse, count);
  (*valdate)[count] = '\0';
  parse += (count + 1);
}

/*-------------------------------------------------------------------------*/

static
int crypt_getloginfo(const char* user, char** passwd, char** valuntil) {

  char*     pwd;
  char*     valdate;
  void*     fakeout;

  *passwd = NULL;
  *valuntil = NULL;
  crypt_loadpwdfile();

  if (pwd_cache) {
    char**    pwd_entry;
    char      user_search[NAMEDATALEN + 2];

    sprintf(user_search, "%s\t", user);
    fakeout = (void*)&user_search;
    if ((pwd_entry = (char**)bsearch((void*)&fakeout, (void*)pwd_cache, pwd_cache_count, sizeof(char*), compar_user))) {
      crypt_parsepwdentry(*pwd_entry, &pwd, &valdate);
      *passwd = pwd;
      *valuntil = valdate;
      return STATUS_OK;
    }

    return STATUS_OK;
  }

  return STATUS_ERROR;
}

/*-------------------------------------------------------------------------*/

#ifdef 0
MsgType crypt_salt(const char* user) {

  char*     passwd;
  char*     valuntil;

  if (crypt_getloginfo(user, &passwd, &valuntil) == STATUS_ERROR)
    return STARTUP_UNSALT_MSG;

  if (passwd == NULL || *passwd == '\0' || !strcmp(passwd, "\\N")) {
    if (passwd) free((void*)passwd);
    if (valuntil) free((void*)valuntil);
    return STARTUP_UNSALT_MSG;
  }

  free((void*)passwd);
  if (valuntil) free((void*)valuntil);
  return STARTUP_SALT_MSG;
}
#endif

/*-------------------------------------------------------------------------*/

int crypt_verify(Port* port, const char* user, const char* pgpass) {

  char*            passwd;
  char*            valuntil;
  char*            crypt_pwd;
  int              retval = STATUS_ERROR;
  AbsoluteTime     vuntil,
                   current;

  if (crypt_getloginfo(user, &passwd, &valuntil) == STATUS_ERROR)
    return STATUS_ERROR;

  if (passwd == NULL || *passwd == '\0') {
    if (passwd) free((void*)passwd);
    if (valuntil) free((void*)valuntil);
    return STATUS_ERROR;
  }

  /*
   * Compare with the encrypted or plain password depending on the
   * authentication method being used for this connection.
   */

  crypt_pwd = (port->auth_method == uaCrypt ? crypt(passwd, port->salt) : passwd);

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
