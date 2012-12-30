#ifndef _LINUX_PHDUSA_H
#define _LINUX_PHDUSA_H

/*
 * kernel/phdusa.h
 *
 * Physical driven User Space Allocator (PhDUSA) interface
 */

#include <linux/types.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>

#ifdef CONFIG_CGROUP_PHDUSA

struct phdusa {
	struct cgroup_subsys_state css;
	unsigned long colormap; /* allowed color bitmap.  */
};

/* Retrieve the phdusa group corresponding to this cgroup container */
struct phdusa *cgroup_ph(struct cgroup *cgrp);

/* Retrieve the phdusa group corresponding to this subsys */
struct phdusa * ph_from_subsys(struct cgroup_subsys_state * subsys);

#endif /* CONFIG_CGROUP_PHDUSA */

#endif /* _LINUX_PHDUSA_H */
