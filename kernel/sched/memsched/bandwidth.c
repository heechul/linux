/**
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/

/**************************************************************************
 * Included Files
 **************************************************************************/
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/

/**************************************************************************
 * Public Types
 **************************************************************************/

/**************************************************************************
 * Global Variables
 **************************************************************************/

int g_cache_line_size = 64;
    /* cat /sys/devices/system/cpu/cpu0/cache/level0/coherency_line_size */
int g_mem_size = 8192 * 1024;
int *g_mem_ptr = 0;
FILE *g_fd = NULL;
char *g_label = NULL;

volatile unsigned long long g_nread = 0;
volatile unsigned int g_start;

/**************************************************************************
 * Public Function Prototypes
 **************************************************************************/

unsigned int get_usecs()
{
	static struct timeval  base;
	struct timeval         time;
	gettimeofday(&time, NULL);
	if (!base.tv_usec) {
		base = time;
	}
	if (base.tv_usec > time.tv_usec)
		return ((time.tv_sec - 1 - base.tv_sec) * 1000000 +
			(1000000 + time.tv_usec - base.tv_usec));
	else
		return ((time.tv_sec - base.tv_sec) * 1000000 +
			(time.tv_usec - base.tv_usec));
}

void quit(int param)
{
	float dur_in_sec;
	float bw;
	unsigned int dur = get_usecs() - g_start;
	dur_in_sec = (float)dur / 1000000;
	printf("g_nread = %lld\n", g_nread);
	printf("elapsed = %.2f sec (%u usec)\n", dur_in_sec, dur);
	bw = (float)g_nread / dur_in_sec / 1024 / 1024;
	printf("B/W = %.2f MB/s\n", bw);

	if (g_fd) fprintf(g_fd, "%s %d\n", g_label, (int)bw);
	fclose(g_fd);

	_exit(0);
}

int bench_read()
{
	int i;
	int sum = 0;    
	register int length = g_mem_size;
	register int line = g_cache_line_size;
	register int *ptr = g_mem_ptr;
	for ( i = 0; i < length/4; i += (line/4)) {
		sum += ptr[i];
		g_nread += line;
	}
	return sum;
}

int bench_write()
{
	int i;
	register int length = g_mem_size;
	register int line = g_cache_line_size;
	register int *ptr = g_mem_ptr;
	
	for ( i = 0; i < length/4; i += (line/4)) {
		ptr[i] = i;
		g_nread += line;
	}
	return 1;
}
int bench_rdwr()
{
	int i;
	int sum = 0;    
	register int length = g_mem_size;
	register int line = g_cache_line_size;
	register int *ptr = g_mem_ptr;

	for ( i = 0; i < length/4; i += (line/4)) {
		ptr[i] = i;
		sum += ptr[i];
		g_nread += line;
	}
	return sum;
}

int bench_worst()
{
	int i;
	int sum = 0;
	register int length = g_mem_size;
	register int line = g_cache_line_size;
	register int *ptr = g_mem_ptr;

	for ( i = 0; i < length/4; i += (line*2)) {
		ptr[i] = i;
		sum += ptr[i];
		g_nread += line;
	}
	return sum;
}

enum access_type { READ, WRITE, RDWR, WRST};

int main(int argc, char *argv[])
{
	unsigned long long sum = 0;
	unsigned finish = 5;
	int cpuid = 0;
	int prio = 0;
        cpu_set_t cmask;
	int num_processors;
	int acc_type = WRST;
	int opt;

	while ((opt = getopt(argc, argv, "m:a:t:c:p:f:l:")) != -1) {
		switch (opt) {
		case 'm':
			g_mem_size = 1024 * strtol(optarg, NULL, 0);
			break;
		case 'a':
			if (!strcmp(optarg, "read"))
				acc_type = READ;
			else if (!strcmp(optarg, "write"))
				acc_type = WRITE;
			else if (!strcmp(optarg, "rdwr"))
				acc_type = RDWR;
			else if (!strcmp(optarg, "worst"))
				acc_type = WRST;
			else
				exit(1);
			break;
		case 't':
			finish = strtol(optarg, NULL, 0);
			break;
		case 'c':
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;
		case 'p':
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		case 'l':
			g_label = strdup(optarg);
			break;

		case 'f':
			g_fd = fopen(optarg, "a+");
			if (g_fd == NULL) 
				perror("error");
			break;
		}
	}

	g_mem_ptr = (int *)malloc(g_mem_size);
	memset(g_mem_ptr, 1, g_mem_size);

	printf("memsize=%d KB, type=%s, cpuid=%d\n",
	       g_mem_size/1024,
	       ((acc_type==READ) ?"read":
		(acc_type==WRITE)? "write" :
		(acc_type==RDWR) ? "rdwr" : "worst"),
		cpuid);
	printf("stop at %d\n", finish);

	signal(SIGINT, &quit);
	signal(SIGALRM, &quit);
	alarm(finish);

	g_start = get_usecs();

	while (1) {
		int sum = 0;
		switch (acc_type) {
		case READ:
			sum = bench_read();
			break;
		case WRITE:
			sum = bench_write();
			break;
		case RDWR:
			sum = bench_rdwr();
			break;
		case WRST:
			sum = bench_worst();
			break;
		}
	}
	printf("sum: %lld\n", sum);
	printf("total read bytes: %lld\n", g_nread);
}
