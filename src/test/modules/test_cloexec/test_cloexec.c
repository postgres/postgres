/*-------------------------------------------------------------------------
 *
 * test_cloexec.c
 *		Test O_CLOEXEC flag handling on Windows
 *
 * This program tests that:
 * 1. File handles opened with O_CLOEXEC are NOT inherited by child processes
 * 2. File handles opened without O_CLOEXEC ARE inherited by child processes
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>

#ifdef WIN32
#include <windows.h>
#endif

#include "common/file_utils.h"
#include "port.h"

#ifdef WIN32
static void run_parent_tests(const char *testfile1, const char *testfile2);
static void run_child_tests(const char *handle1_str, const char *handle2_str);
static bool try_write_to_handle(HANDLE h, const char *label);
#endif

int
main(int argc, char *argv[])
{
	/* Windows-only test */
#ifndef WIN32
	fprintf(stderr, "This test only runs on Windows\n");
	return 0;
#else
	char		testfile1[MAXPGPATH];
	char		testfile2[MAXPGPATH];

	if (argc == 3)
	{
		/*
		 * Child mode: receives two handle values as hex strings and attempts
		 * to write to them.
		 */
		run_child_tests(argv[1], argv[2]);
		return 0;
	}
	else if (argc == 1)
	{
		/* Parent mode: opens files and spawns child */
		snprintf(testfile1, sizeof(testfile1), "test_cloexec_1_%d.tmp", (int) getpid());
		snprintf(testfile2, sizeof(testfile2), "test_cloexec_2_%d.tmp", (int) getpid());

		run_parent_tests(testfile1, testfile2);

		/* Clean up test files */
		unlink(testfile1);
		unlink(testfile2);

		return 0;
	}
	else
	{
		fprintf(stderr, "Usage: %s [handle1_hex handle2_hex]\n", argv[0]);
		return 1;
	}
#endif
}

#ifdef WIN32
static void
run_parent_tests(const char *testfile1, const char *testfile2)
{
	int			fd1,
				fd2;
	HANDLE		h1,
				h2;
	char		exe_path[MAXPGPATH];
	char		cmdline[MAXPGPATH + 100];
	STARTUPINFO si = {.cb = sizeof(si)};
	PROCESS_INFORMATION pi = {0};
	DWORD		exit_code;

	printf("Parent: Opening test files...\n");

	/* Open first file WITH O_CLOEXEC - should NOT be inherited */
	fd1 = open(testfile1, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd1 < 0)
	{
		fprintf(stderr, "Failed to open %s: %s\n", testfile1, strerror(errno));
		exit(1);
	}

	/* Open second file WITHOUT O_CLOEXEC - should be inherited */
	fd2 = open(testfile2, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd2 < 0)
	{
		fprintf(stderr, "Failed to open %s: %s\n", testfile2, strerror(errno));
		close(fd1);
		exit(1);
	}

	/* Get Windows HANDLEs from file descriptors */
	h1 = (HANDLE) _get_osfhandle(fd1);
	h2 = (HANDLE) _get_osfhandle(fd2);

	if (h1 == INVALID_HANDLE_VALUE || h2 == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Failed to get OS handles\n");
		close(fd1);
		close(fd2);
		exit(1);
	}

	printf("Parent: fd1=%d (O_CLOEXEC) -> HANDLE=%p\n", fd1, h1);
	printf("Parent: fd2=%d (no O_CLOEXEC) -> HANDLE=%p\n", fd2, h2);

	/*
	 * Find the actual executable path by removing any arguments from
	 * GetCommandLine(), and add our new arguments.
	 */
	GetModuleFileName(NULL, exe_path, sizeof(exe_path));
	snprintf(cmdline, sizeof(cmdline), "\"%s\" %p %p", exe_path, h1, h2);

	printf("Parent: Spawning child process...\n");
	printf("Parent: Command line: %s\n", cmdline);

	if (!CreateProcess(NULL,	/* application name */
					   cmdline, /* command line */
					   NULL,	/* process security attributes */
					   NULL,	/* thread security attributes */
					   TRUE,	/* bInheritHandles - CRITICAL! */
					   0,		/* creation flags */
					   NULL,	/* environment */
					   NULL,	/* current directory */
					   &si,		/* startup info */
					   &pi))	/* process information */
	{
		fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
		close(fd1);
		close(fd2);
		exit(1);
	}

	printf("Parent: Waiting for child process...\n");

	/* Wait for child to complete */
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &exit_code);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	close(fd1);
	close(fd2);

	printf("Parent: Child exit code: %lu\n", exit_code);

	if (exit_code == 0)
	{
		printf("Parent: SUCCESS - O_CLOEXEC behavior verified\n");
	}
	else
	{
		printf("Parent: FAILURE - O_CLOEXEC not working correctly\n");
		exit(1);
	}
}

static void
run_child_tests(const char *handle1_str, const char *handle2_str)
{
	HANDLE		h1,
				h2;
	bool		h1_worked,
				h2_worked;

	/* Parse handle values from hex strings */
	if (sscanf(handle1_str, "%p", &h1) != 1 ||
		sscanf(handle2_str, "%p", &h2) != 1)
	{
		fprintf(stderr, "Child: Failed to parse handle values\n");
		exit(1);
	}

	printf("Child: Received HANDLE1=%p (should fail - O_CLOEXEC)\n", h1);
	printf("Child: Received HANDLE2=%p (should work - no O_CLOEXEC)\n", h2);

	/* Try to write to both handles */
	h1_worked = try_write_to_handle(h1, "HANDLE1");
	h2_worked = try_write_to_handle(h2, "HANDLE2");

	printf("Child: HANDLE1 (O_CLOEXEC): %s\n",
		   h1_worked ? "ACCESSIBLE (BAD!)" : "NOT ACCESSIBLE (GOOD!)");
	printf("Child: HANDLE2 (no O_CLOEXEC): %s\n",
		   h2_worked ? "ACCESSIBLE (GOOD!)" : "NOT ACCESSIBLE (BAD!)");

	/*
	 * For O_CLOEXEC to work correctly, h1 should NOT be accessible (h1_worked
	 * == false) and h2 SHOULD be accessible (h2_worked == true).
	 */
	if (!h1_worked && h2_worked)
	{
		printf("Child: Test PASSED - O_CLOEXEC working correctly\n");
		exit(0);
	}
	else
	{
		printf("Child: Test FAILED - O_CLOEXEC not working correctly\n");
		exit(1);
	}
}

static bool
try_write_to_handle(HANDLE h, const char *label)
{
	const char *test_data = "test\n";
	DWORD		bytes_written;
	BOOL		result;

	result = WriteFile(h, test_data, strlen(test_data), &bytes_written, NULL);

	if (result && bytes_written == strlen(test_data))
	{
		printf("Child: Successfully wrote to %s\n", label);
		return true;
	}
	else
	{
		printf("Child: Failed to write to %s (error %lu)\n",
			   label, GetLastError());
		return false;
	}
}
#endif
