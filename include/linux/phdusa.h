#ifndef _LINUX_PHDUSA_H
#define _LINUX_PHDUSA_H

/*
 * kernel/phdusa.h
 *
 * cache color and DRAM aware page allocator
 */

#include <linux/types.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/mm.h>

#ifdef CONFIG_CGROUP_PHDUSA

#define USE_DRAM_AWARE 1 /* heechul for dram aware allocation */

extern int sysctl_cache_color_bits;

extern int sysctl_dram_rank_bits;
extern int sysctl_dram_bank_bits;

extern int sysctl_dram_rank_shift;
extern int sysctl_dram_bank_shift;

#if USE_DRAM_AWARE
#  define paddr_to_dram_rank(paddr) \
	(((unsigned)(paddr) >> sysctl_dram_rank_shift) & ((1 << sysctl_dram_rank_bits) - 1))
#  define paddr_to_dram_bank(paddr) \
	(((unsigned)(paddr) >> sysctl_dram_bank_shift) & ((1 << sysctl_dram_bank_bits) - 1))
#  define paddr_to_color(paddr) (						\
	(paddr_to_dram_rank(paddr) << (sysctl_dram_bank_bits)) |		\
	(paddr_to_dram_bank(paddr)))

#  define page_to_color(page) paddr_to_color(page_to_phys(page))
#  define dram_addr_to_color(rank, bank)	(		\
	rank << sysctl_dram_bank_bits |	bank)

#  define COLOR_TO_DRAM_RANK(c) \
	((c >> sysctl_dram_bank_bits) & ((1<<sysctl_dram_rank_bits)-1))
#  define COLOR_TO_DRAM_BANK(c) \
	(c & ((1<<sysctl_dram_bank_bits)-1))
#else
#  define page_to_color(page) (page_to_pfn(page) % (1<<sysctl_cache_color_bits))
#endif

struct phdusa {
	struct cgroup_subsys_state css;
	unsigned long colormap; /* allowed color bitmap.  */
#if USE_DRAM_AWARE
	unsigned long dram_bankmap;
	unsigned long dram_rankmap;
#endif
};

/* Retrieve the phdusa group corresponding to this cgroup container */
struct phdusa *cgroup_ph(struct cgroup *cgrp);

/* Retrieve the phdusa group corresponding to this subsys */
struct phdusa * ph_from_subsys(struct cgroup_subsys_state * subsys);

#endif /* CONFIG_CGROUP_PHDUSA */

#endif /* _LINUX_PHDUSA_H */
