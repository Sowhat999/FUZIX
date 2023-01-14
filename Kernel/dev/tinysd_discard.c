/*
 *	Boot time code for tinysd
 */

#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <tinysd.h>
#include "mbr.h"

#ifdef CONFIG_DYNAMIC_SWAP
static void swap_found(uint_fast8_t minor, partition_table_entry_t *pe)
{
    uint32_t off;
    uint16_t n = 0;
    if (swap_dev != 0xFFFF)
        return;
    kputs("(swap) ");
    swap_dev = minor;		/* major is 0 */
    off = le32_to_cpu(pe->lba_count);
    
    while(off > SWAP_SIZE && n < MAX_SWAPS) {
        off -= SWAP_SIZE;
        n++;
    }
    while(n)
        swapmap_init(--n);
}
#endif

static uint_fast8_t setup(uint16_t dev)
{
    uint32_t *lba = sd_lba[dev];
    uint_fast8_t n = 0;
    uint_fast8_t c = 0;
    boot_record_t *br = (boot_record_t *)tmpbuf();
    partition_table_entry_t *pe = br->partition;
    udata.u_block = 0;
    udata.u_nblock = 1;
    udata.u_dptr = (void *)br;
    if (sd_read(dev << 4, 0, 0) != BLKSIZE) {
        tmpfree(br);
        return 0;
    }
    kprintf("hd%c: ", 'a' + dev);

    if (le16_to_cpu(br->signature) == MBR_SIGNATURE) {
        while(n < 4) {
            if (pe->type_chs_last[0]) {
                kprintf("hd%c%d ", 'a' + dev, ++c); 
                *++lba = le32_to_cpu(pe->lba_first);   
            }
#ifdef CONFIG_DYNAMIC_SWAP    
            if (pe->type_chs_last[0] == FUZIX_SWAP)
                swap_found((dev << 4) | c, pe);
#endif            
            n++;
            pe++;
        }
    }
    tmpfree(br);
    return 1;
}

void sd_setup(uint_fast8_t dev)
{
    uint_fast8_t n = setup(dev);
    kputchar('\n');
    if (n == 0) {
        sd_shift[dev] = 0xFF;	/* Disable */ 
        return;
    }
}
