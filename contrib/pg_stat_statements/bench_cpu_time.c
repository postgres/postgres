/*
 * bench_cpu_time.c
 *
 * Benchmark to demonstrate the difference between wall clock time
 * and CPU time (via getrusage) under varying system load.
 *
 * Under no load: wall ~= cpu
 * Under CPU saturation (e.g. stress-ng): wall >> cpu (scheduling delay)
 * Under I/O saturation: wall >> cpu (I/O wait)
 *
 * Usage:
 *   cc -O2 -o bench_cpu_time bench_cpu_time.c -lm
 *   ./bench_cpu_time [iterations]
 *
 * Then compare with:
 *   stress-ng --cpu $(nproc) --timeout 30s &
 *   ./bench_cpu_time
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define DEFAULT_ITERATIONS 10
#define WORK_SIZE 5000000  /* number of math ops per iteration */

static double
wall_time_ms(struct timespec *start, struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0
		 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static double
rusage_cpu_ms(struct rusage *start, struct rusage *end)
{
	double user_ms = (end->ru_utime.tv_sec - start->ru_utime.tv_sec) * 1000.0
				   + (end->ru_utime.tv_usec - start->ru_utime.tv_usec) / 1000.0;
	double sys_ms  = (end->ru_stime.tv_sec - start->ru_stime.tv_sec) * 1000.0
				   + (end->ru_stime.tv_usec - start->ru_stime.tv_usec) / 1000.0;
	return user_ms + sys_ms;
}

/*
 * Pure CPU-bound work: compute a bunch of sqrt/sin to keep the CPU busy.
 * volatile to prevent the compiler from optimizing it away.
 */
static volatile double sink;

static void
cpu_bound_work(void)
{
	double acc = 0.0;

	for (int i = 0; i < WORK_SIZE; i++)
		acc += sqrt((double)i) * sin((double)i);

	sink = acc;
}

/*
 * Mixed I/O + CPU work: write to a temp file between CPU bursts.
 */
static void
io_bound_work(void)
{
	char buf[4096];
	char path[] = "/tmp/bench_cpu_time_XXXXXX";
	int fd;
	double acc = 0.0;

	memset(buf, 'x', sizeof(buf));
	fd = mkstemp(path);
	if (fd < 0)
	{
		perror("mkstemp");
		return;
	}
	unlink(path);

	for (int i = 0; i < WORK_SIZE / 10; i++)
	{
		acc += sqrt((double)i);
		if (i % 1000 == 0)
		{
			write(fd, buf, sizeof(buf));
			fsync(fd);
		}
	}

	close(fd);
	sink = acc;
}

static void
run_benchmark(const char *label, void (*workfn)(void), int iterations)
{
	double total_wall = 0.0;
	double total_cpu = 0.0;

	printf("\n=== %s (%d iterations) ===\n", label, iterations);
	printf("%4s  %10s  %10s  %10s  %7s\n",
		   "#", "wall(ms)", "cpu(ms)", "off-cpu(ms)", "cpu%");

	for (int i = 0; i < iterations; i++)
	{
		struct timespec ts_start, ts_end;
		struct rusage ru_start, ru_end;

		getrusage(RUSAGE_SELF, &ru_start);
		clock_gettime(CLOCK_MONOTONIC, &ts_start);

		workfn();

		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		getrusage(RUSAGE_SELF, &ru_end);

		double wall = wall_time_ms(&ts_start, &ts_end);
		double cpu  = rusage_cpu_ms(&ru_start, &ru_end);
		double off  = wall - cpu;
		double pct  = (wall > 0) ? (cpu / wall) * 100.0 : 0.0;

		printf("%4d  %10.2f  %10.2f  %10.2f  %6.1f%%\n",
			   i + 1, wall, cpu, off, pct);

		total_wall += wall;
		total_cpu  += cpu;
	}

	double avg_wall = total_wall / iterations;
	double avg_cpu  = total_cpu / iterations;
	double avg_off  = avg_wall - avg_cpu;
	double avg_pct  = (avg_wall > 0) ? (avg_cpu / avg_wall) * 100.0 : 0.0;

	printf("----  ----------  ----------  ----------  -------\n");
	printf(" avg  %10.2f  %10.2f  %10.2f  %6.1f%%\n",
		   avg_wall, avg_cpu, avg_off, avg_pct);
}

int
main(int argc, char *argv[])
{
	int iterations = DEFAULT_ITERATIONS;

	if (argc > 1)
		iterations = atoi(argv[1]);
	if (iterations < 1)
		iterations = 1;

	printf("PID: %d\n", getpid());
	printf("Work size: %d ops per iteration\n", WORK_SIZE);

	run_benchmark("CPU-bound work", cpu_bound_work, iterations);
	run_benchmark("I/O-bound work", io_bound_work, iterations);

	return 0;
}
