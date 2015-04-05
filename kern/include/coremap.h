//
//  coremap.h
//  cs350Proj
//
//  Created by Nicholas Lauer on 2015-04-05.
//  Copyright (c) 2015 Nicholas Lauer. All rights reserved.
//

#ifndef __cs350Proj__coremap__
#define __cs350Proj__coremap__

#include "opt-A3.h"

void cm_bootstrap(void);
paddr_t cm_getppages(unsigned long npages);
vaddr_t cm_alloc_kpages(int npages);
void cm_free_kpages(vaddr_t addr);

#endif /* defined(__cs350Proj__coremap__) */
