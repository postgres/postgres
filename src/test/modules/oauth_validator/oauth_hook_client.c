/*-------------------------------------------------------------------------
 *
 * oauth_hook_client.c
 *		Test driver for t/002_client.pl, which verifies OAuth hook
 *		functionality in libpq.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		src/test/modules/oauth_validator/oauth_hook_client.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/socket.h>

#include "getopt_long.h"
#include "libpq-fe.h"

static int	handle_auth_data(PGauthData type, PGconn *conn, void *data);
static PostgresPollingStatusType async_cb(PGconn *conn,
										  PGoauthBearerRequest *req,
										  pgsocket *altsock);
static PostgresPollingStatusType misbehave_cb(PGconn *conn,
											  PGoauthBearerRequest *req,
											  pgsocket *altsock);

static void
usage(char *argv[])
{
	printf("usage: %s [flags] CONNINFO\n\n", argv[0]);

	printf("recognized flags:\n");
	printf("  -h, --help              show this message\n");
	printf("  -v VERSION              select the hook API version (default 2)\n");
	printf("  --expected-scope SCOPE  fail if received scopes do not match SCOPE\n");
	printf("  --expected-uri URI      fail if received configuration link does not match URI\n");
	printf("  --expected-issuer ISS   fail if received issuer does not match ISS (v2 only)\n");
	printf("  --misbehave=MODE        have the hook fail required postconditions\n"
		   "                          (MODEs: no-hook, fail-async, no-token, no-socket)\n");
	printf("  --no-hook               don't install OAuth hooks\n");
	printf("  --hang-forever          don't ever return a token (combine with connect_timeout)\n");
	printf("  --token TOKEN           use the provided TOKEN value\n");
	printf("  --error ERRMSG          fail instead, with the given ERRMSG (v2 only)\n");
	printf("  --stress-async          busy-loop on PQconnectPoll rather than polling\n");
}

/* --options */
static bool no_hook = false;
static bool hang_forever = false;
static bool stress_async = false;
static const char *expected_uri = NULL;
static const char *expected_issuer = NULL;
static const char *expected_scope = NULL;
static const char *misbehave_mode = NULL;
static char *token = NULL;
static char *errmsg = NULL;
static int	hook_version = PQAUTHDATA_OAUTH_BEARER_TOKEN_V2;

int
main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},

		{"expected-scope", required_argument, NULL, 1000},
		{"expected-uri", required_argument, NULL, 1001},
		{"no-hook", no_argument, NULL, 1002},
		{"token", required_argument, NULL, 1003},
		{"hang-forever", no_argument, NULL, 1004},
		{"misbehave", required_argument, NULL, 1005},
		{"stress-async", no_argument, NULL, 1006},
		{"expected-issuer", required_argument, NULL, 1007},
		{"error", required_argument, NULL, 1008},
		{0}
	};

	const char *conninfo;
	PGconn	   *conn;
	int			c;

	while ((c = getopt_long(argc, argv, "hv:", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'h':
				usage(argv);
				return 0;

			case 'v':
				if (strcmp(optarg, "1") == 0)
					hook_version = PQAUTHDATA_OAUTH_BEARER_TOKEN;
				else if (strcmp(optarg, "2") == 0)
					hook_version = PQAUTHDATA_OAUTH_BEARER_TOKEN_V2;
				else
				{
					usage(argv);
					return 1;
				}
				break;

			case 1000:			/* --expected-scope */
				expected_scope = optarg;
				break;

			case 1001:			/* --expected-uri */
				expected_uri = optarg;
				break;

			case 1002:			/* --no-hook */
				no_hook = true;
				break;

			case 1003:			/* --token */
				token = optarg;
				break;

			case 1004:			/* --hang-forever */
				hang_forever = true;
				break;

			case 1005:			/* --misbehave */
				misbehave_mode = optarg;
				break;

			case 1006:			/* --stress-async */
				stress_async = true;
				break;

			case 1007:			/* --expected-issuer */
				expected_issuer = optarg;
				break;

			case 1008:			/* --error */
				errmsg = optarg;
				break;

			default:
				usage(argv);
				return 1;
		}
	}

	if (argc != optind + 1)
	{
		usage(argv);
		return 1;
	}

	conninfo = argv[optind];

	/* Set up our OAuth hooks. */
	PQsetAuthDataHook(handle_auth_data);

	/* Connect. (All the actual work is in the hook.) */
	if (stress_async)
	{
		/*
		 * Perform an asynchronous connection, busy-looping on PQconnectPoll()
		 * without actually waiting on socket events. This stresses code paths
		 * that rely on asynchronous work to be done before continuing with
		 * the next step in the flow.
		 */
		PostgresPollingStatusType res;

		conn = PQconnectStart(conninfo);

		do
		{
			res = PQconnectPoll(conn);
		} while (res != PGRES_POLLING_FAILED && res != PGRES_POLLING_OK);
	}
	else
	{
		/* Perform a standard synchronous connection. */
		conn = PQconnectdb(conninfo);
	}

	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "connection to database failed: %s\n",
				PQerrorMessage(conn));
		PQfinish(conn);
		return 1;
	}

	printf("connection succeeded\n");
	PQfinish(conn);
	return 0;
}

/*
 * PQauthDataHook implementation. Replaces the default client flow by handling
 * PQAUTHDATA_OAUTH_BEARER_TOKEN[_V2].
 */
static int
handle_auth_data(PGauthData type, PGconn *conn, void *data)
{
	PGoauthBearerRequest *req;
	PGoauthBearerRequestV2 *req2 = NULL;

	Assert(hook_version == PQAUTHDATA_OAUTH_BEARER_TOKEN ||
		   hook_version == PQAUTHDATA_OAUTH_BEARER_TOKEN_V2);

	if (no_hook || type != hook_version)
		return 0;

	req = data;
	if (type == PQAUTHDATA_OAUTH_BEARER_TOKEN_V2)
		req2 = data;

	if (hang_forever)
	{
		/* Start asynchronous processing. */
		req->async = async_cb;
		return 1;
	}

	if (misbehave_mode)
	{
		if (strcmp(misbehave_mode, "no-hook") != 0)
			req->async = misbehave_cb;
		return 1;
	}

	if (expected_uri)
	{
		if (!req->openid_configuration)
		{
			fprintf(stderr, "expected URI \"%s\", got NULL\n", expected_uri);
			return -1;
		}

		if (strcmp(expected_uri, req->openid_configuration) != 0)
		{
			fprintf(stderr, "expected URI \"%s\", got \"%s\"\n", expected_uri, req->openid_configuration);
			return -1;
		}
	}

	if (expected_scope)
	{
		if (!req->scope)
		{
			fprintf(stderr, "expected scope \"%s\", got NULL\n", expected_scope);
			return -1;
		}

		if (strcmp(expected_scope, req->scope) != 0)
		{
			fprintf(stderr, "expected scope \"%s\", got \"%s\"\n", expected_scope, req->scope);
			return -1;
		}
	}

	if (expected_issuer)
	{
		if (!req2)
		{
			fprintf(stderr, "--expected-issuer cannot be combined with -v1\n");
			return -1;
		}

		if (!req2->issuer)
		{
			fprintf(stderr, "expected issuer \"%s\", got NULL\n", expected_issuer);
			return -1;
		}

		if (strcmp(expected_issuer, req2->issuer) != 0)
		{
			fprintf(stderr, "expected issuer \"%s\", got \"%s\"\n", expected_issuer, req2->issuer);
			return -1;
		}
	}

	if (errmsg)
	{
		if (token)
		{
			fprintf(stderr, "--error cannot be combined with --token\n");
			return -1;
		}
		else if (!req2)
		{
			fprintf(stderr, "--error cannot be combined with -v1\n");
			return -1;
		}

		req2->error = errmsg;
		return -1;
	}

	req->token = token;
	return 1;
}

static PostgresPollingStatusType
async_cb(PGconn *conn, PGoauthBearerRequest *req, pgsocket *altsock)
{
	if (hang_forever)
	{
		/*
		 * This code tests that nothing is interfering with libpq's handling
		 * of connect_timeout.
		 */
		static pgsocket sock = PGINVALID_SOCKET;

		if (sock == PGINVALID_SOCKET)
		{
			/* First call. Create an unbound socket to wait on. */
#ifdef WIN32
			WSADATA		wsaData;
			int			err;

			err = WSAStartup(MAKEWORD(2, 2), &wsaData);
			if (err)
			{
				perror("WSAStartup failed");
				return PGRES_POLLING_FAILED;
			}
#endif
			sock = socket(AF_INET, SOCK_DGRAM, 0);
			if (sock == PGINVALID_SOCKET)
			{
				perror("failed to create datagram socket");
				return PGRES_POLLING_FAILED;
			}
		}

		/* Make libpq wait on the (unreadable) socket. */
		*altsock = sock;
		return PGRES_POLLING_READING;
	}

	req->token = token;
	return PGRES_POLLING_OK;
}

static PostgresPollingStatusType
misbehave_cb(PGconn *conn, PGoauthBearerRequest *req, pgsocket *altsock)
{
	if (strcmp(misbehave_mode, "fail-async") == 0)
	{
		/* Just fail "normally". */
		if (errmsg)
		{
			PGoauthBearerRequestV2 *req2;

			if (hook_version == PQAUTHDATA_OAUTH_BEARER_TOKEN)
			{
				fprintf(stderr, "--error cannot be combined with -v1\n");
				exit(1);
			}

			req2 = (PGoauthBearerRequestV2 *) req;
			req2->error = errmsg;
		}

		return PGRES_POLLING_FAILED;
	}
	else if (strcmp(misbehave_mode, "no-token") == 0)
	{
		/* Callbacks must assign req->token before returning OK. */
		return PGRES_POLLING_OK;
	}
	else if (strcmp(misbehave_mode, "no-socket") == 0)
	{
		/* Callbacks must assign *altsock before asking for polling. */
		return PGRES_POLLING_READING;
	}
	else
	{
		fprintf(stderr, "unrecognized --misbehave mode: %s\n", misbehave_mode);
		exit(1);
	}
}
