/* src/include/port/win32_msvc/unistd.h */

/*
 * MSVC does not define these, nor does _fileno(stdin) etc reliably work
 * (returns -1 if stdin/out/err are closed).
 */
#define STDIN_FILENO	0
#define STDOUT_FILENO	1
#define STDERR_FILENO	2
