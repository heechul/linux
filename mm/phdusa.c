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
#include <linux/bitmap.h>
#include <linux/module.h>

/*
 * Check if a page is compliant to the policy defined for the given vma
 */
#ifdef CONFIG_CGROUP_PHDUSA

#define MAX_LINE_LEN (6*128)
/*
 * Types of files in a phdusa group
 * FILE_COLORS - contain list of colors allowed
 * FILE_PROFILE - contains the profile information for the task
*/
typedef enum {
#if USE_DRAM_AWARE
	FILE_DRAM_RANK,
	FILE_DRAM_BANK,
#endif
	FILE_COLORS,
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

/*
 * Common write function for files in phdusa cgroup
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


static int phdusa_file_write(struct cgroup *cgrp, struct cftype *cft,
			     const char *buf)
{
	int retval = 0;
	struct phdusa *ph = cgroup_ph(cgrp);
	int bank, rank, color;

	if (!cgroup_lock_live_group(cgrp))
		return -ENODEV;

	switch (cft->private) {
#if USE_DRAM_AWARE
	case FILE_DRAM_RANK:
		retval = update_bitmask(&ph->dram_rankmap, buf, 1<<sysctl_dram_rank_bits); /* up to 4 ranks */
		printk(KERN_INFO "Rank : %s\n", buf);
		break;
	case FILE_DRAM_BANK:
		retval = update_bitmask(&ph->dram_bankmap, buf, 1<<sysctl_dram_bank_bits); /* up to 8 banks */
		printk(KERN_INFO "Bank : %s\n", buf);
		break;
#endif
	case FILE_COLORS:
		retval = update_bitmask(&ph->color_map, buf, 1<<sysctl_cache_color_bits);
		break;
	default:
		retval = -EINVAL;
		break;
	}

	/* repopulate the bin map */

	bitmap_zero(ph->cmap, MAX_CACHE_BINS);
	for_each_set_bit(rank, &ph->dram_rankmap,
			 1<<sysctl_dram_rank_bits) {
		for_each_set_bit(bank, &ph->dram_bankmap,
				 1<<sysctl_dram_bank_bits) {
			for_each_set_bit(color, &ph->color_map,
					 1<<sysctl_cache_color_bits) {
				bitmap_set(ph->cmap,
					   dram_addr_to_color(
						   rank, bank, color),
					   1);
			}
		}
	}
	
	cgroup_unlock();
	return retval;
}

static ssize_t phdusa_file_read(struct cgroup *cgrp,
				struct cftype *cft,
				struct file *file,
				char __user *buf,
				size_t nbytes, loff_t *ppos)
{
	struct phdusa *ph = cgroup_ph(cgrp);
	char *page;
	ssize_t retval = 0;
	char *s;

	if (!(page = (char *)__get_free_page(GFP_TEMPORARY)))
		return -ENOMEM;

	s = page;

	switch (cft->private) {
#if USE_DRAM_AWARE
	case FILE_DRAM_RANK:
		s += bitmap_scnlistprintf(s, PAGE_SIZE, &ph->dram_rankmap, 1<<sysctl_dram_rank_bits);
		printk(KERN_INFO "Rank : %s\n", s);
		break;
	case FILE_DRAM_BANK:
		s += bitmap_scnlistprintf(s, PAGE_SIZE, &ph->dram_bankmap, 1<<sysctl_dram_bank_bits);
		printk(KERN_INFO "Bank : %s\n", s);
		break;
#endif
	case FILE_COLORS:
		s += bitmap_scnlistprintf(s, PAGE_SIZE, &ph->color_map, 1<<sysctl_cache_color_bits);
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
#if USE_DRAM_AWARE
	{
		.name = "dram_rank",
		.read = phdusa_file_read,
		.write_string = phdusa_file_write,
		.max_write_len = MAX_LINE_LEN,
		.private = FILE_DRAM_RANK,
	},
	{
		.name = "dram_bank",
		.read = phdusa_file_read,
		.write_string = phdusa_file_write,
		.max_write_len = MAX_LINE_LEN,
		.private = FILE_DRAM_BANK,
	},
#endif
	{
		.name = "colors",
		.read = phdusa_file_read,
		.write_string = phdusa_file_write,
		.max_write_len = MAX_LINE_LEN,
		.private = FILE_COLORS,
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

	printk(KERN_INFO "Creating the new cgroup - %p\n", cgrp);

	if (!cgrp->parent) {
		return &top_phdusa.css;
	}
	ph_parent = cgroup_ph(cgrp->parent);

	ph_child = kmalloc(sizeof(struct phdusa), GFP_KERNEL);
	if(!ph_child)
		return ERR_PTR(-ENOMEM);

	bitmap_clear(&ph_child->color_map, 0, 1<<sysctl_cache_color_bits);
#if USE_DRAM_AWARE
	bitmap_clear(&ph_child->dram_rankmap, 0, 1<<sysctl_dram_rank_bits);
	bitmap_clear(&ph_child->dram_bankmap, 0, 1<<sysctl_dram_bank_bits);
#endif
	bitmap_clear(ph_child->cmap, 0, MAX_CACHE_BINS);
	return &ph_child->css;
}


/*
 * Destroy an existing phdusa group
 */
static void phdusa_destroy(struct cgroup *cgrp)
{
	struct phdusa *ph = cgroup_ph(cgrp);
	printk(KERN_INFO "Deleting the cgroup - %p\n",cgrp);
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
