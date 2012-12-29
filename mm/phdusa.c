/*
 * kernel/phdusa.c
 *
 * Physical driven User Space Allocator info for a set of tasks.
 */

#include <linux/types.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/phdusa.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/fs.h>

/*
 * Policy compliancy checking procedure
 */
int check_policy_page(struct list_head * phinfo, struct page * page){
	struct list_head * phentry;
	unsigned long pfn;
	
	if(!phinfo || list_empty(phinfo)) return 1;
	
	pfn = page_to_pfn(page);
	list_for_each(phentry, phinfo){
		unsigned long phmask = 
			list_entry(phentry, struct phinfo, list)->phmask;
		unsigned long phpattern = 
			list_entry(phentry, struct phinfo, list)->phpattern;
		
		if(~(~(pfn ^ phpattern) | ~phmask) == 0)
			return 1;
	}
	return 0;
}

/*
 * Check of a page is compliant to the policy defined for the given vma
 */
int check_policy_vma(struct page * page, struct vm_area_struct * vma){
	struct list_head * phinfo = NULL;

	if (vma && vma->vm_mm == current->mm){
		phinfo = &(ph_from_subsys(current->cgroups->
					  subsys[phdusa_subsys_id])->policy);
	}

	return check_policy_page(phinfo, page);
}

#ifdef CONFIG_CGROUP_PHDUSA

/* 
 * Types of files in a phdusa group 
 * FILE_PHPATTERN - Contains unsigned long representing physical pattern
 * FILE_PHMASK - Contains unsigned long representing physical mask
*/
typedef enum {
	FILE_PHPATTERN,
	FILE_PHMASK,
} phdusa_filetype_t;

/*
 * Top level phdusa - mask initialized to zero implying no restriction on 
 * physical pages
*/

static struct phdusa top_phdusa;

/* Retrieve the phdusa group corresponding to this cgroup container */
struct phdusa *cgroup_ph(struct cgroup *cgrp)
{
	return container_of(cgroup_subsys_state(cgrp, phdusa_subsys_id),
			    struct phdusa, css);
}

struct phdusa * ph_from_subsys(struct cgroup_subsys_state * subsys)
{
	return container_of(subsys, struct phdusa, css);
}

/* refer cpuset.c for handling read/write */
static ssize_t phdusa_file_read(struct cgroup *cgrp, 
				struct cftype *cfg,
				struct file *file,
				char __user *buf,
				size_t nbytes, 
				loff_t *ppos)
{
	struct phdusa *ph = cgroup_ph(cgrp);
	struct list_head *curr;
	char *page;
	ssize_t retval = 0;
	char *s;

	printk(KERN_INFO "Reading policy list from cgroup - %p\n", cgrp);

	if (!(page = (char *)__get_free_page(GFP_TEMPORARY)))
		return -ENOMEM;

	s = page;

	list_for_each(curr, &ph->policy) {
		struct phinfo *info;
		info = list_entry(curr, struct phinfo, list);
		s += sprintf(s, "0x%08lx 0x%08lx\n", info->phmask, info->phpattern);
	}

	*s++ = '\n';

	retval = simple_read_from_buffer(buf, nbytes, ppos, page, s - page);
	free_page((unsigned long)page);
	return retval;
}


static void update_phpattern(struct phdusa* ph, u64 val)
{
	struct phinfo * phinfo_new;
	phinfo_new = kmalloc(sizeof(struct phinfo), GFP_KERNEL);
	phinfo_new->phpattern = (unsigned long)val;
	phinfo_new->phmask = 0;
	list_add(&phinfo_new->list, &ph->policy);
}

static void update_phmask(struct phdusa* ph, u64 val)
{
	struct phinfo * phinfo_last = list_entry(ph->policy.next, struct phinfo, list);
	phinfo_last->phmask = (unsigned long)val;
}

/*
 * Common write function for files in phdusa cgroup
 */
static int phdusa_common_file_write(struct cgroup *cgrp,
				    struct cftype *cft,
				    u64 val)
{
	int retval = 0;
	struct phdusa *ph = cgroup_ph(cgrp);
	phdusa_filetype_t type = cft->private;

	printk(KERN_INFO "Writing file %d of cgroup - %p\n", type, cgrp);
	
	if (!cgroup_lock_live_group(cgrp))
		return -ENODEV;

	switch (type) {
	case FILE_PHPATTERN:
		update_phpattern(ph, val);
		break;
	case FILE_PHMASK:
		update_phmask(ph, val);
		break;
	default:
		retval = -EINVAL;
		break;
	}
	cgroup_unlock();
	return retval;
}

/*
 * Common read function for files in phdusa cgroup
 */
static u64 phdusa_common_file_read(struct cgroup *cgrp,
				       struct cftype *cft)
{
	int retval = 0;	
	struct phdusa *ph = cgroup_ph(cgrp);
	phdusa_filetype_t type = cft->private;
	struct phinfo * phinfo_new;

	printk(KERN_INFO "Reading file %d of cgroup - %p\n", type, cgrp);

	if(!list_empty(&ph->policy))
		phinfo_new = list_entry(ph->policy.next, struct phinfo, list);
	else return 0;

	switch (type) {
	case FILE_PHPATTERN:
		retval = phinfo_new->phpattern;
		break;
	case FILE_PHMASK:
		retval = phinfo_new->phmask;
		break;
	default:
		retval = -EINVAL;
		break;
	}

	return retval;
}

/*
 * struct cftype: handler definitions for cgroup control files
 *
 * for the common functions, 'private' gives the type of the file
 */

static struct cftype files[] = {
	{
		.name = "control",
		.read = phdusa_file_read,
		.private = 0,
	},
	{
		.name = "phys_pattern",
		.read_u64 = phdusa_common_file_read,
		.write_u64 = phdusa_common_file_write,
		.private = FILE_PHPATTERN,
	},
	{
		.name = "phys_mask",
		.read_u64 = phdusa_common_file_read,
		.write_u64 = phdusa_common_file_write,
		.private = FILE_PHMASK,
	},

	{ }	/* terminate */
};


/*
 * phdusa_create - create a phdusa group
 */
static struct cgroup_subsys_state *phdusa_create(struct cgroup *cgrp)
{
	struct phdusa * ph_child;
	struct phdusa * ph_parent;
	struct list_head * curr;
	struct phinfo * phinfo_curr;
	struct phinfo * phinfo_new;
	
	printk(KERN_INFO "Creating the new cgroup - %p\n",cgrp);

	if(!cgrp->parent) {
		INIT_LIST_HEAD(&top_phdusa.policy);
		return &top_phdusa.css; 
	}
	ph_parent = cgroup_ph(cgrp->parent);

	ph_child = kmalloc(sizeof(struct phdusa), GFP_KERNEL);
	if(!ph_child)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ph_child->policy);

	/* Copy the policy from parent to child */
	list_for_each(curr, &ph_parent->policy){
		phinfo_curr = list_entry(curr, struct phinfo, list);
		phinfo_new = kmalloc(sizeof(struct phinfo), GFP_KERNEL);
		phinfo_new->phpattern = phinfo_curr->phpattern;
		phinfo_new->phmask = phinfo_curr->phmask;
		list_add(&phinfo_new->list, &ph_child->policy);
	}

	return &ph_child->css;
}

/*
 * Destroy an existing phdusa group
 */
static void phdusa_destroy(struct cgroup *cgrp)
{
	struct phdusa *ph = cgroup_ph(cgrp);
	struct list_head * list_item, * list_tmp;
	struct phinfo * curr_phinfo;
	printk(KERN_INFO "Deleting the cgroup - %p\n",cgrp);

	/* Destroy the policy */	
	list_for_each_safe(list_item, list_tmp, &ph->policy){
		curr_phinfo = list_entry(list_item, struct phinfo, list);
		list_del(list_item);
		kfree(curr_phinfo);
	}

	kfree(ph);
}

struct cgroup_subsys phdusa_subsys = {
	.name = "phdusa",
	.create = phdusa_create,
	.destroy = phdusa_destroy,
	.subsys_id = phdusa_subsys_id,
	.base_cftypes = files,
};

#endif /* CONFIG_CGROUP_PHDUSA */
