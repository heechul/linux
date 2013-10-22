/*
 * kernel/phalloc.c
 *
 * Physical driven User Space Allocator info for a set of tasks.
 */

#include <linux/types.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/phalloc.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/bitmap.h>
#include <linux/module.h>

/*
 * Check if a page is compliant to the policy defined for the given vma
 */
#ifdef CONFIG_CGROUP_PHALLOC

#define MAX_LINE_LEN (6*128)
/*
 * Types of files in a phalloc group
 * FILE_PHALLOC - contain list of phalloc bins allowed
*/
typedef enum {
	FILE_PHALLOC,
} phalloc_filetype_t;

/*
 * Top level phalloc - mask initialized to zero implying no restriction on
 * physical pages
*/

static struct phalloc top_phalloc;

/* Retrieve the phalloc group corresponding to this cgroup container */
struct phalloc *cgroup_ph(struct cgroup *cgrp)
{
	return container_of(cgroup_subsys_state(cgrp, phalloc_subsys_id),
			    struct phalloc, css);
}

struct phalloc * ph_from_subsys(struct cgroup_subsys_state * subsys)
{
	return container_of(subsys, struct phalloc, css);
}

/*
 * Common write function for files in phalloc cgroup
 */
static int update_bitmask(unsigned long *bitmap, const char *buf, int maxbits)
{
	int retval = 0;

	if (!*buf)
		bitmap_clear(bitmap, 0, maxbits);
	else
		retval = bitmap_parselist(buf, bitmap, maxbits);

	return retval;
}


static int phalloc_file_write(struct cgroup *cgrp, struct cftype *cft,
			     const char *buf)
{
	int retval = 0;
	struct phalloc *ph = cgroup_ph(cgrp);

	if (!cgroup_lock_live_group(cgrp))
		return -ENODEV;

	switch (cft->private) {
	case FILE_PHALLOC:
		retval = update_bitmask(ph->cmap, buf, phalloc_bins());
		printk(KERN_INFO "Bins : %s\n", buf);
		break;
	default:
		retval = -EINVAL;
		break;
	}

	cgroup_unlock();
	return retval;
}

static ssize_t phalloc_file_read(struct cgroup *cgrp,
				struct cftype *cft,
				struct file *file,
				char __user *buf,
				size_t nbytes, loff_t *ppos)
{
	struct phalloc *ph = cgroup_ph(cgrp);
	char *page;
	ssize_t retval = 0;
	char *s;

	if (!(page = (char *)__get_free_page(GFP_TEMPORARY)))
		return -ENOMEM;

	s = page;

	switch (cft->private) {
	case FILE_PHALLOC:
		s += bitmap_scnlistprintf(s, PAGE_SIZE, ph->cmap, phalloc_bins());
		printk(KERN_INFO "Bins : %s\n", s);
		break;
	default:
		retval = -EINVAL;
		goto out;
	}
	*s++ = '\n';

	retval = simple_read_from_buffer(buf, nbytes, ppos, page, s - page);
out:
	free_page((unsigned long)page);
	return retval;
}


/*
 * struct cftype: handler definitions for cgroup control files
 *
 * for the common functions, 'private' gives the type of the file
 */
static struct cftype files[] = {
	{
		.name = "bins",
		.read = phalloc_file_read,
		.write_string = phalloc_file_write,
		.max_write_len = MAX_LINE_LEN,
		.private = FILE_PHALLOC,
	},
	{ }	/* terminate */
};

/*
 * phalloc_create - create a phalloc group
 */
static struct cgroup_subsys_state *phalloc_create(struct cgroup *cgrp)
{
	struct phalloc * ph_child;
	struct phalloc * ph_parent;

	printk(KERN_INFO "Creating the new cgroup - %p\n", cgrp);

	if (!cgrp->parent) {
		return &top_phalloc.css;
	}
	ph_parent = cgroup_ph(cgrp->parent);

	ph_child = kmalloc(sizeof(struct phalloc), GFP_KERNEL);
	if(!ph_child)
		return ERR_PTR(-ENOMEM);

	bitmap_clear(ph_child->cmap, 0, MAX_PHALLOC_BINS);
	return &ph_child->css;
}


/*
 * Destroy an existing phalloc group
 */
static void phalloc_destroy(struct cgroup *cgrp)
{
	struct phalloc *ph = cgroup_ph(cgrp);
	printk(KERN_INFO "Deleting the cgroup - %p\n",cgrp);
	kfree(ph);
}

struct cgroup_subsys phalloc_subsys = {
	.name = "phalloc",
	.create = phalloc_create,
	.destroy = phalloc_destroy,
	.subsys_id = phalloc_subsys_id,
	.base_cftypes = files,
};

#endif /* CONFIG_CGROUP_PHALLOC */
