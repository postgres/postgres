#if defined(__i386__)
#define NEED_I386_TAS_ASM
#endif
#if defined(__sparc__)
#define NEED_SPARC_TAS_ASM
#endif

#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

/* This is marked as obsoleted in BSD/OS 4.3. */
#ifndef EAI_ADDRFAMILY
#define  EAI_ADDRFAMILY		1
#endif
