// userapp.c - CS423 MP2 test app
// Registers itself with /proc/mp2/status, verifies admission,
// yields once, runs a periodic factorial job loop, yields after each job,
// prints wakeup and processing time per iteration, then deregisters.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define PROC_STATUS_PATH "/proc/mp2/status"
#define DEFAULT_PERIOD_MS 1000
#define DEFAULT_COMPUTATION_MS 200
#define DEFAULT_NUM_JOBS 5


/* 
    Print error message and exit
*/
static void error_exit(const char *msg)
{
	fprintf(stderr, "userapp: %s: %s\n", msg, strerror(errno));
	exit(1);
}


/* 
    Write command string to /proc/mp2/status
    Exit on any error
*/
static void write_command(const char *cmd)
{
	FILE *fp;

	fp = fopen(PROC_STATUS_PATH, "w");
	if (!fp)
		error_exit("failed to open " PROC_STATUS_PATH " for write");

	if (fprintf(fp, "%s\n", cmd) < 0) {
		fclose(fp);
		error_exit("failed to write command");
	}

	if (fclose(fp) != 0)
		error_exit("failed to close " PROC_STATUS_PATH " after write");
}


/* 
    Register this process with scheduler via proc filesystem
*/
static void register_self(unsigned long period_ms, unsigned long computation_ms)
{
	char cmd[128];
	pid_t pid;

	pid = getpid();
	snprintf(cmd, sizeof(cmd), "R,%d,%lu,%lu",
		 (int)pid, period_ms, computation_ms);
	write_command(cmd);
}


/* 
    Signal scheduler: this process completes current job and yields
*/
static void yield_self(void)
{
	char cmd[64];
	pid_t pid;

	pid = getpid();
	snprintf(cmd, sizeof(cmd), "Y,%d", (int)pid);
	write_command(cmd);
}


/* 
    Deregister this process from scheduler
*/
static void deregister_self(void)
{
	char cmd[64];
	pid_t pid;

	pid = getpid();
	snprintf(cmd, sizeof(cmd), "D,%d", (int)pid);
	write_command(cmd);
}


/* 
    Check if this process is listed in /proc/mp2/status
    Returns 1 if registered, 0 if not
*/
static int is_registered(void)
{
	FILE *fp;
	char line[256];
	pid_t pid;
	int listed_pid;
	unsigned long listed_period;
	unsigned long listed_computation;

	pid = getpid();

	fp = fopen(PROC_STATUS_PATH, "r");
	if (!fp)
		error_exit("failed to open " PROC_STATUS_PATH " for read");

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "%d: %lu, %lu",
			   &listed_pid, &listed_period, &listed_computation) == 3) {
			if (listed_pid == pid) {
				if (fclose(fp) != 0)
					error_exit("failed to close " PROC_STATUS_PATH " after read");
				return 1;
			}
		}
	}

	if (ferror(fp)) {
		fclose(fp);
		error_exit("error while reading " PROC_STATUS_PATH);
	}

	if (fclose(fp) != 0)
		error_exit("failed to close " PROC_STATUS_PATH " after read");

	return 0;
}


/* 
    Compute intensive workload: factorial calculations
    Used to measure job processing time
*/
static void do_job(void)
{
	volatile unsigned long long sum;
	int i;
	int j;

	sum = 0;

	for (i = 0; i < 100000; i++) {
		volatile unsigned long long fac = 1;

		for (j = 1; j <= 30; j++)
			fac *= (unsigned long long)j;

		sum += fac;
	}

	(void)sum;
}


/* 
    Convert timespec to milliseconds
*/
static double timespec_to_ms(const struct timespec *ts)
{
	return (double)ts->tv_sec * 1000.0 + (double)ts->tv_nsec / 1000000.0;
}

/* 
    Compute elapsed time between two timestamps in milliseconds
*/
static double timespec_diff_ms(const struct timespec *start,
			       const struct timespec *end)
{
	return timespec_to_ms(end) - timespec_to_ms(start);
}


/* 
    Periodic real time test application
    Registers with scheduler, runs num_jobs iterations with yields between,
    measures wakeup and processing times, then deregisters
*/
int main(int argc, char *argv[])
{
	unsigned long period_ms;
	unsigned long computation_ms;
	int num_jobs;
	int job;
	struct timespec t0;
	struct timespec wake_time;
	struct timespec end_time;
	double wakeup_ms;
	double process_ms;

	period_ms = DEFAULT_PERIOD_MS;
	computation_ms = DEFAULT_COMPUTATION_MS;
	num_jobs = DEFAULT_NUM_JOBS;

	if (argc >= 2)
		period_ms = strtoul(argv[1], NULL, 10);
	if (argc >= 3)
		computation_ms = strtoul(argv[2], NULL, 10);
	if (argc >= 4)
		num_jobs = atoi(argv[3]);

	if (period_ms == 0 || computation_ms == 0 || num_jobs <= 0) {
		fprintf(stderr, "usage: %s [period_ms] [computation_ms] [num_jobs]\n",
			argv[0]);
		return 1;
	}

	register_self(period_ms, computation_ms);

	if (!is_registered()) {
		fprintf(stderr, "userapp: registration rejected\n");
		return 1;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0)
		error_exit("clock_gettime failed before RT loop");

	yield_self();

	for (job = 0; job < num_jobs; job++) {
		if (clock_gettime(CLOCK_MONOTONIC, &wake_time) != 0)
			error_exit("clock_gettime failed at job start");

		do_job();

		if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0)
			error_exit("clock_gettime failed at job end");

		wakeup_ms = timespec_diff_ms(&t0, &wake_time);
		process_ms = timespec_diff_ms(&wake_time, &end_time);

		printf("job %d: wakeup_time=%.3f ms process_time=%.3f ms\n",
		       job + 1, wakeup_ms, process_ms);
		fflush(stdout);

		yield_self();
	}

	deregister_self();
	return 0;
}