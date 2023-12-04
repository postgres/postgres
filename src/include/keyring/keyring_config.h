
#ifndef KEYRING_CONFIG_H
#define KEYRING_CONFIG_H

#include "postgres.h"

#include <json.h>

extern char* keyringConfigFile;
extern char* keyringKeyPrefix;

void keyringRegisterVariables(void);

bool keyringLoadConfiguration(const char* configFileName);


// If it's a hash, tries to retrieve the remote value
// { type: 'remote'. url: 'http://...' }
// If it doesn't have a type key / not remote / ... returns NULL
// Otherwise it retuns the JSON value interpreted as a string
const char* keyringParseStringParam(json_object* object);

#endif // KEYRING_CONFIG_H
