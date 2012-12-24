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

struct phinfo {
	unsigned long phpattern; /* pattern of the physical page */
	unsigned long phmask;	 /* bitmask for the pattern */
        struct list_head list;	 /* allow several patterns */
};

struct phdusa {
       struct cgroup_subsys_state css;
       struct list_head policy;
};


/* Retrieve the phdusa group corresponding to this cgroup container */
struct phdusa *cgroup_ph(struct cgroup *cgrp);

/* Retrieve the phdusa group corresponding to this subsys */
struct phdusa * ph_from_subsys(struct cgroup_subsys_state * subsys);

/*
 * Policy compliancy checking procedure
 */
int check_policy_page(struct list_head * phinfo, struct page * page);

/*
 * Check of a page is compliant to the policy defined for the given vma
 */
int check_policy_vma(struct page * page, struct vm_area_struct * vma);

#endif /* CONFIG_CGROUP_PHDUSA */

#endif /* _LINUX_PHDUSA_H */
