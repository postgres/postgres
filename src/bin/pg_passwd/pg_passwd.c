/*
 * @(#) pg_passwd.c 1.8 09:13:16 97/07/02     Y. Ichikawa
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <ctype.h>
#define issaltchar(c) (isalnum(c) || (c) == '.' || (c) == '/')
#include "postgres.h"
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#else
extern char   *crypt(const char *, const char *);
#endif
char  *comname;
void usage(FILE *stream);
void read_pwd_file(char *filename);
void write_pwd_file(char *filename, char *bkname);
void encrypt_pwd(char key[9], char salt[3], char passwd[14]);
int check_pwd(char key[9], char passwd[14]);
void prompt_for_username(char *username);
void prompt_for_password(char *prompt, char *password);
void usage(FILE *stream)
{
      fprintf(stream, "Usage: %s <password file>\n", comname);
}
typedef struct {
      char    *uname;
      char    *pwd;
      char    *rest;
} pg_pwd;
#define MAXPWDS       1024
pg_pwd        pwds[MAXPWDS];
int   npwds = 0;
void read_pwd_file(char *filename)
{
    FILE      *fp;
    static char       line[512];
    static char ans[128];
    int               i;
  try_again:
    fp = fopen(filename, "r");
    if (fp == NULL) {
      if (errno == ENOENT) {
          printf("File \"%s\" does not exist.  Create? (y/n): ", filename);
          fflush(stdout);
          fgets(ans, 128, stdin);
          switch (ans[0]) {
            case 'y': case 'Y':
              fp = fopen(filename, "w");
              if (fp == NULL) {
                  perror(filename);
                  exit(1);
              }
              fclose(fp);
              goto try_again;
            default:
              /* cannot continue */
              exit(1);
          }
      } else {
          perror(filename);
          exit(1);
      }
    }
    /* read all the entries */
    for (npwds = 0; npwds < MAXPWDS && fgets(line, 512, fp) != NULL; ++npwds)
