//
//  coremap.c
//  cs350Proj
//
//  Created by Nicholas Lauer on 2015-04-05.
//  Copyright (c) 2015 Nicholas Lauer. All rights reserved.
//

#include <types.h>
#include <lib.h>
#include "coremap.h"
#include "spinlock.h"
#include "vm.h"

struct coremap_entry {
    paddr_t paddr;
    paddr_t kvaddr;
    uint32_t segmentLength;
    bool isUsed;
};

static struct coremap_entry *coremap;
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
static bool vm_initialized = false;
static uint32_t first_coremap_page = -1;
static uint32_t last_coremap_page = -1;

static void cm_initialize_coremap()
{
    KASSERT(first_coremap_page > 0);
    KASSERT(last_coremap_page > 0);
    
    for (uint32_t i = 0; i < (last_coremap_page - first_coremap_page); i++) {
        coremap[i].isUsed = false;
        coremap[i].paddr = first_coremap_page*PAGE_SIZE + i*PAGE_SIZE;
        coremap[i].kvaddr = PADDR_TO_KVADDR(coremap[i].paddr);
        coremap[i].segmentLength = 0;
    }
}

void cm_bootstrap(void)
{
    paddr_t lo;
    paddr_t hi;
    uint32_t npages;
    uint32_t coremapSize;
    
    ram_getsize(&lo, &hi);
    
    DEBUG(DB_VM, "low: 0x%x, hi: 0x%x\n", lo,hi);
    
    // Calculate the number of pages available at this time
    npages = (hi - lo) / PAGE_SIZE;
    
    DEBUG(DB_VM, "Pages Available: %u\n", npages);
    
    // We can't call kmalloc for the coremap's space as stated in the hint since there is no more mem after ram_getsize
    // So we need to allocate it ourselves
    coremapSize = npages * sizeof(struct coremap_entry);
    
    // We need to claim it as pages, so round up the size to the nearest page
    coremapSize = ROUNDUP(coremapSize, PAGE_SIZE);
    
    coremap = (struct coremap_entry *)PADDR_TO_KVADDR(lo);
    lo += coremapSize;
    
    DEBUG(DB_VM, "Pages Available after coremap created: %u\n", (hi - lo) / PAGE_SIZE);
    
    // Make lo and high correspond to pages, and store them as the edges of the coremap
    first_coremap_page = lo / PAGE_SIZE;
    last_coremap_page = hi / PAGE_SIZE;
    
    cm_initialize_coremap();
    
    vm_initialized = true;
    return;
}

paddr_t cm_getppages(unsigned long npages)
{
    paddr_t addr;
    
    if (vm_initialized) {
        // Need to find npages free pages in the coremap. Search from the front for a block of the right length
        spinlock_acquire(&coremap_lock);
        DEBUG(DB_VM, "Asked for npages: %lu\n", npages);
        bool startSet = false;
        uint32_t start = 0;
        uint32_t found = 0;
        uint32_t numcoremap = last_coremap_page - first_coremap_page;
        for (uint32_t i = 0; i < numcoremap; i++) {
            if (coremap[i].isUsed == true) {
                start = 0;
                found = 0;
                startSet = false;
            } else {
                if (startSet == false) {
                    start = i;
                    startSet = true;
                }
                found++;
                
                // We found a block that is long enough to give, so return the starting paddr
                if (found == npages) {
                    DEBUG(DB_VM, "Found npages free starting at: %u\n", start);
                    // We are giving these pages back, so we should make them as used
                    for (uint32_t k = start; k < start + npages; k++) {
                        coremap[k].isUsed = true;
                        coremap[k].segmentLength = npages - (k - start);
                    }
                    DEBUG(DB_VM, "Returning paddr for npages: 0x%x\n", coremap[start].paddr);
                    spinlock_release(&coremap_lock);
                    return coremap[start].paddr;
                }
            }
        }
        
        for (uint32_t i = 0; i < numcoremap; i++) {
            DEBUG(DB_VM, "Page State: %u: %u\n", i, coremap[i].isUsed);
        }
        spinlock_release(&coremap_lock);
        panic("There aren't any free pages\n");
    } else {
        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    }
    
    return addr;
}

vaddr_t cm_alloc_kpages(int npages)
{
    paddr_t pa;
    pa = cm_getppages(npages);
    if (pa==0) {
        return 0;
    }
    
    return PADDR_TO_KVADDR(pa);
}

void cm_free_kpages(vaddr_t addr)
{
    if (vm_initialized) {
        spinlock_acquire(&coremap_lock);
        paddr_t paddr = addr - MIPS_KSEG0;
        DEBUG(DB_VM, "Asked to free VADDR: 0x%x, PADDR: 0x%x\n", addr, paddr);
        
        uint32_t coremapIndex = paddr / PAGE_SIZE - first_coremap_page;
        
        uint32_t segmentLength = coremap[coremapIndex].segmentLength;
        DEBUG(DB_VM, "Freeing segment at index: %u length: %u\n", coremapIndex, segmentLength);
        
        for (uint32_t i = coremapIndex; i < coremapIndex + segmentLength; i++) {
            DEBUG(DB_VM, "Freeing segment at index: %u\n", i);
            coremap[i].segmentLength = 0;
            coremap[i].isUsed = false;
        }
        
        spinlock_release(&coremap_lock);
    }
}
