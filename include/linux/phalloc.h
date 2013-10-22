#ifndef _LINUX_PHALLOC_H
#define _LINUX_PHALLOC_H

/*
 * kernel/phalloc.h
 *
 * PHysical memory aware allocator
 */

#include <linux/types.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/mm.h>

#ifdef CONFIG_CGROUP_PHALLOC

struct phalloc {
	struct cgroup_subsys_state css;
	COLOR_BITMAP(cmap);
};

/* Retrieve the phalloc group corresponding to this cgroup container */
struct phalloc *cgroup_ph(struct cgroup *cgrp);

/* Retrieve the phalloc group corresponding to this subsys */
struct phalloc * ph_from_subsys(struct cgroup_subsys_state * subsys);

/* return #of phalloc bins */
int phalloc_bins(void);

#endif /* CONFIG_CGROUP_PHALLOC */

#endif /* _LINUX_PHALLOC_H */
