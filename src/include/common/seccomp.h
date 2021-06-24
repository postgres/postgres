#ifndef PG_SECCOMP_H
#define PG_SECCOMP_H

#include "postgres.h"

#ifdef HAVE_LIBSECCOMP
#include <seccomp.h>
#endif

typedef struct {
    int syscall;    /* syscall number */
    uint32 action;  /* libseccomp action, e.g. SCMP_ACT_ALLOW */
} PgSeccompRule;

#define PG_SCMP(syscall_, action_) \
    (PgSeccompRule) { .syscall = SCMP_SYS(syscall_), .action = action_ }

#define PG_SCMP_ALLOW(syscall_) \
    PG_SCMP(syscall_, SCMP_ACT_ALLOW)

int seccomp_load_rules(PgSeccompRule *syscalls, int count);
void test_seccomp(void);

#endif /* PG_SECCOMP_H */
