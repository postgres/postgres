#ifndef PASSWORD_H
#define PASSWORD_H

#include <libpq/hba.h>
#include <libpq/pqcomm.h>

#define PWFILE_NAME_SIZE USERMAP_NAME_SIZE

int
verify_password(char *user, char *password, Port *port,
				char *database, char *DataDir);

#endif
