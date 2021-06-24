#include "postgres.h"
#include "miscadmin.h"
#include "common/seccomp.h"

#include <unistd.h>

/*
 * Enter seccomp mode with a BPF filter that will only allow
 * certain syscalls to proceed.
 */
int
seccomp_load_rules(PgSeccompRule *syscalls, int count) {
	scmp_filter_ctx ctx;
	int rc;
	int i;

	/* By default, any syscall not in the list will crash the process */
	if ((ctx = seccomp_init(SCMP_ACT_KILL_PROCESS)) == NULL)
		return -1;

	for (i = 0; i < count; i++) {
		PgSeccompRule *rule = &syscalls[i];
		if ((rc = seccomp_rule_add(ctx, rule->action, rule->syscall, 0)) != 0)
			return -1;
	}

	if ((rc = seccomp_load(ctx)) != 0)
		return -1;

	return 0;
}

static void
write_str(char *s)
{
	/* Best effort write to stderr */
	(void)write(STDERR_FILENO, s, strlen(s));
}

static void
bail(int code, char *s) {
	write_str(s);

	/* XXX: we don't want to run any atexit callbacks */
	_exit(code);
}

static void
test_seccomp_sighandler(int signum, siginfo_t *info, void *cxt)
{
	if (signum != SIGSYS)
		bail(1, "bad signal number\n");

	/* TODO: maybe somehow extract the hardcoded syscall number */
	if (info->_sifields._sigsys._syscall != __NR_brk)
		bail(1, "bad syscall number\n");

	bail(0, "seccomp tests have passed\n");
}

void
test_seccomp(void)
{
	struct sigaction action =
	{
		.sa_sigaction = test_seccomp_sighandler,
		.sa_flags = SA_SIGINFO,
	};

	PgSeccompRule syscalls[] =
	{
		PG_SCMP(exit_group, SCMP_ACT_ALLOW),
		PG_SCMP(write, SCMP_ACT_ALLOW),
		PG_SCMP(brk, SCMP_ACT_TRAP),
	};

	/* XXX: pqsignal() is too restrictive for our purposes */
	if (sigaction(SIGSYS, &action, NULL) != 0)
		bail(1, "failed to install sigsys signal handler\n");

	/* XXX: this time we'd like to receive a real SIGSYS */
	if (seccomp_load_rules(syscalls, lengthof(syscalls)) != 0)
		bail(1, "failed to enter seccomp bpf mode\n");

	/* test the good syscall */
	write_str("write seems to work\n");

	/*
	 * Test the bad syscall.
	 * XXX: this should be the last check.
	 */
	brk(NULL);

	bail(1, "unreachable\n");
}
