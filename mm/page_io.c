/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <asm/pgtable.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

/*
 * nvm-swap: swap count information system-calls
 */
static int swap_outs = 0;
static int swap_ins = 0;
SYSCALL_DEFINE0(reset_swap_outs)
{
	swap_outs = 0;
	swap_ins = 0;
	return 0;
}
SYSCALL_DEFINE0(print_swap_outs)
{
	/*
	printk(KERN_ERR "[SWAP] swap-outs = %d, swap-ins = %d\n",
			swap_outs, swap_ins);
	*/
	return swap_outs;
}
SYSCALL_DEFINE0(print_swap_ins)
{
	return swap_ins;
}

static struct bio *get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);
	if (bio) {
		bio->bi_sector = map_swap_page(page, &bio->bi_bdev);
		bio->bi_sector <<= PAGE_SHIFT - 9;
		bio->bi_io_vec[0].bv_page = page;
		bio->bi_io_vec[0].bv_len = PAGE_SIZE;
		bio->bi_io_vec[0].bv_offset = 0;
		bio->bi_vcnt = 1;
		bio->bi_idx = 0;
		bio->bi_size = PAGE_SIZE;
		bio->bi_end_io = end_io;
	}
	return bio;
}

static void end_swap_bio_write(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (!uptodate) {
		SetPageError(page);
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid rotate_reclaimable_page()
		 */
		set_page_dirty(page);
		printk(KERN_ALERT "Write-error on swap-device (%u:%u:%Lu)\n",
				imajor(bio->bi_bdev->bd_inode),
				iminor(bio->bi_bdev->bd_inode),
				(unsigned long long)bio->bi_sector);
		ClearPageReclaim(page);
	}
	end_page_writeback(page);
	bio_put(bio);
}

void end_swap_bio_read(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (!uptodate) {
		SetPageError(page);
		ClearPageUptodate(page);
		printk(KERN_ALERT "Read-error on swap-device (%u:%u:%Lu)\n",
				imajor(bio->bi_bdev->bd_inode),
				iminor(bio->bi_bdev->bd_inode),
				(unsigned long long)bio->bi_sector);
	} else {
		SetPageUptodate(page);
	}
	unlock_page(page);
	bio_put(bio);
}

#ifdef CONFIG_MEMSWAP
/*
 * nvm-swap
 */
void mem_swap_writepage(struct page *page, struct swap_info_struct *si)
{
    swp_entry_t entry;
    pgoff_t offset, old;
    unsigned long pfn;
    void *pg_addr, *swp_addr;

    entry.val = page_private(page);
    old = swp_offset(entry);
    offset = si->slot_map[old];

    pfn = offset + si->start_pfn;
    swp_addr = __va(pfn << PAGE_SHIFT);
    pg_addr = kmap_atomic(page);

    memcpy(swp_addr, pg_addr, PAGE_SIZE);

    si->slot_age[offset]++;
    min_heapify(si->heap, si->index, si->max, si->index[offset]);

    kunmap_atomic(pg_addr);
}
#endif

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
int swap_writepage(struct page *page, struct writeback_control *wbc)
{
	struct bio *bio;
	int ret = 0, rw = WRITE;
	struct swap_info_struct *si;

	if (try_to_free_swap(page)) {
		unlock_page(page);
		goto out;
	}

#ifdef CONFIG_MEMSWAP
	/* nvm-swap: do memory swap */
	si = mem_swap_page2info(page);
	if (si->flags & SWP_MEM) {
		count_vm_event(PSWPOUT);
		set_page_writeback(page);
		unlock_page(page);
		mem_swap_writepage(page, si);
		end_page_writeback(page);
		goto out;
	}
#endif

	bio = get_swap_bio(GFP_NOIO, page, end_swap_bio_write);
	if (bio == NULL) {
		set_page_dirty(page);
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	if (wbc->sync_mode == WB_SYNC_ALL)
		rw |= REQ_SYNC;
	count_vm_event(PSWPOUT);
	swap_outs++;
	set_page_writeback(page);
	unlock_page(page);
	submit_bio(rw, bio);
out:
	return ret;
}

#ifdef CONFIG_MEMSWAP
/*
 * nvm-swap
 */
void mem_swap_readpage(struct page *page, struct swap_info_struct *si)
{
	swp_entry_t entry;
	pgoff_t offset;
	unsigned long pfn;
	void *pg_addr, *swp_addr;

	entry.val = page_private(page);
	offset = swp_offset(entry);
	offset = si->slot_map[offset];

	pfn = offset + si->start_pfn;
	swp_addr = __va(pfn << PAGE_SHIFT);
	pg_addr = kmap_atomic(page);

	memcpy(pg_addr, swp_addr, PAGE_SIZE);

	kunmap_atomic(pg_addr);
}
#endif

int swap_readpage(struct page *page)
{
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *si;

	VM_BUG_ON(!PageLocked(page));
	VM_BUG_ON(PageUptodate(page));

#ifdef CONFIG_MEMSWAP
	si = mem_swap_page2info(page);
	if (si->flags & SWP_MEM) {
		count_vm_event(PSWPIN);
		mem_swap_readpage(page, si);
		unlock_page(page);
		SetPageUptodate(page);
		goto out;
	}
#endif

	bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);
	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	count_vm_event(PSWPIN);
	swap_ins++;
	submit_bio(READ, bio);
out:
	return ret;
}
