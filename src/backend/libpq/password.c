#include <postgres.h>
#include <libpq/password.h>
#include <libpq/hba.h>
#include <libpq/libpq.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif

int
verify_password(char *user, char *password, Port *port, 
		char *database, char *DataDir)
{
    bool host_ok;
    enum Userauth userauth;
    char pw_file_name[PWFILE_NAME_SIZE+1];

    char *pw_file_fullname;
    FILE *pw_file;

    char pw_file_line[255];
    char *p, *test_user, *test_pw;
    char salt[3];

    find_hba_entry(DataDir, port->raddr.sin_addr, database, 
		   &host_ok, &userauth, pw_file_name, true);

    if(!host_ok) {
	sprintf(PQerrormsg,
		"verify_password: couldn't find entry for connecting host\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);	
	return STATUS_ERROR;
    }

    if(userauth != Password) {
	sprintf(PQerrormsg,
		"verify_password: couldn't find entry of type 'password' "
		"for this host\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);	
	return STATUS_ERROR;
    }

    if(!pw_file_name || pw_file_name[0] == '\0') {
	sprintf(PQerrormsg,
		"verify_password: no password file specified\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);	
	return STATUS_ERROR;
    }

    pw_file_fullname = (char *)malloc(strlen(DataDir) + strlen(pw_file_name) + 2);
    strcpy(pw_file_fullname, DataDir);
    strcat(pw_file_fullname, "/");
    strcat(pw_file_fullname, pw_file_name);

    pw_file = fopen(pw_file_fullname, "r");
    if(!pw_file) {
	sprintf(PQerrormsg,
		"verify_password: couldn't open password file '%s'\n",
		pw_file_fullname);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);	
	return STATUS_ERROR;
    }

    while(!feof(pw_file)) {
	fgets(pw_file_line, 255, pw_file);
	p = pw_file_line;

	test_user = strtok(p, ":");
	test_pw = strtok(NULL, ":");
	if(!test_user || !test_pw ||
	   test_user[0] == '\0' || test_pw[0] == '\0') {
	    continue;
	}

	/* kill the newline */
	test_pw[strlen(test_pw)-1] = '\0';

	strNcpy(salt, test_pw, 2);

	if(strcmp(user, test_user) == 0) {
	    /* we're outta here one way or the other. */
	    fclose(pw_file);

	    if(strcmp(crypt(password, salt), test_pw) == 0) {
		/* it matched. */
		return STATUS_OK;
	    }

	    sprintf(PQerrormsg, 
		    "verify_password: password mismatch for '%s'.\n",
		    user);
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);	
	    return STATUS_ERROR;
	}
    }

    sprintf(PQerrormsg, 
	    "verify_password: user '%s' not found in password file.\n",
	    user);
    fputs(PQerrormsg, stderr);
    pqdebug("%s", PQerrormsg);	
    return STATUS_ERROR;
}
