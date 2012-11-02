/**
 * DRAM access latency measurement program
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
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "list.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64
#define CACHE_LINE_BITS 6

/**************************************************************************
 * Public Types
 **************************************************************************/

struct item {
	int data;
	int in_use;
	struct list_head list;
} __attribute__((aligned(CACHE_LINE_SIZE)));;

/**************************************************************************
 * Global Variables
 **************************************************************************/

int g_mem_size = 8192*1024;

/**************************************************************************
 * Public Function Prototypes
 **************************************************************************/

uint64_t get_elapsed(struct timespec *start, struct timespec *end)
{
	uint64_t dur;
	if (start->tv_nsec > end->tv_nsec)
		dur = (uint64_t)(end->tv_sec - 1 - start->tv_sec) * 1000000000 +
			(1000000000 + end->tv_nsec - start->tv_nsec);
	else
		dur = (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000 +
			(end->tv_nsec - start->tv_nsec);

	return dur;

}

void usage(int argc, char *argv[])
{
	printf("Usage: $ %s [<option>]*\n\n", argv[0]);
	printf("-m: memory size in KB. deafult=8192\n");
	printf("-s: turn serial access mode on\n");
	printf("-c: CPU to run.\n");
	printf("-i: iterations. 0 means intefinite. default=0\n");
	printf("-p: priority\n");
	printf("-h: help\n");
	printf("\nExamples: \n$ bandwidth -m 8192 -a read -t 1 -c 2\n  <- 8MB read for 1 second on CPU 2\n");
	exit(1);
}

int main(int argc, char* argv[])
{
	struct item *list;
	int workingset_size = 1024;
	int i, j;
	struct list_head head;
	struct list_head *pos;
	struct timespec start, end;
	uint64_t nsdiff;
	uint64_t readsum = 0;
	int serial = 0;
	int repeat = 1;
	int cpuid = 0;
	struct sched_param param;
        cpu_set_t cmask;
	int num_processors;
	int opt, prio;
	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "m:sc:i:p:h")) != -1) {
		switch (opt) {
		case 'm': /* set memory size */
			g_mem_size = 1024 * strtol(optarg, NULL, 0);
			break;
		case 's': /* set access type */
			serial = 1;
			break;
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;
			
		case 'p': /* set priority */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		case 'i': /* iterations */
			repeat = strtol(optarg, NULL, 0);
			fprintf(stderr, "repeat=%d\n", repeat);
			break;
		case 'h': 
			usage(argc, argv);
			break;
		}
	}

	workingset_size = g_mem_size / CACHE_LINE_SIZE;
	srand(0);
#if 1
        param.sched_priority = 1;
        if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
		perror("sched_setscheduler failed");
        }
#endif

	INIT_LIST_HEAD(&head);

	/* allocate */
	list = (struct item *)malloc(sizeof(struct item) * workingset_size + CACHE_LINE_SIZE);
	list = (struct item *)
		((((unsigned)list + CACHE_LINE_SIZE) >> CACHE_LINE_BITS) << CACHE_LINE_BITS);

	printf("addr: 0x%x   aligned?:%s\n", (unsigned)list, (((unsigned)list)%64==0)?"yes":"no");
	for (i = 0; i < workingset_size; i++) {
		list[i].data = i;
		list[i].in_use = 0;
		INIT_LIST_HEAD(&list[i].list);
		// printf("%d 0x%x\n", list[i].data, &list[i].data);
	}
	printf("allocated: wokingsetsize=%d entries\n", workingset_size);

	/* initialize */
	i = workingset_size;
	while (i > 0) {
		int idx;
		int j;
		if (serial)
			idx = workingset_size - i;
		else
			idx = rand() % workingset_size;

		for (j = idx; j < idx + workingset_size; j++) {
			int idx2 = j % workingset_size;
			if (!list[idx2].in_use) {
				list_add(&list[idx2].list, &head);
				list[idx2].in_use = 1;
				i--;
				break;
			}
		}
	}
	printf("initialized\n");

	/* actual access */
	clock_gettime(CLOCK_REALTIME, &start);
	for (j = 0; j < repeat; j++) {
		list_for_each(pos, &head) {
			struct item *tmp = list_entry(pos, struct item, list);
			readsum += tmp->data;
			// printf("%d ", tmp->data, &tmp->data);
		}
	}
	clock_gettime(CLOCK_REALTIME, &end);

	nsdiff = get_elapsed(&start, &end);

	printf("duration %lldus\naverage %lldns\n", nsdiff/1000, nsdiff/workingset_size/repeat);
	printf("readsum  %lld\n", readsum);
}

