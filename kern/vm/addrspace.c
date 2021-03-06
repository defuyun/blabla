/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *        The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>



/* takes in an address space address and an entry_hi,
 * returns a hashed index within the HPT */
uint32_t hpt_hash(struct addrspace *as, vaddr_t vpn) {
        uint32_t index;
        index = (((uint32_t )as) ^ (vpn >> PAGE_BITS)) % hpt_size;
        return index;
}


struct hpt_entry * find(struct addrspace * as, vaddr_t vpn) {

        uint32_t pid = (uint32_t) as;
        int index = hpt_hash(as, vpn);

        struct hpt_entry * ptr = hpt[index];
        while (ptr != NULL) {
                if ((ptr->pid == pid) &&
                    (vpn == (ptr->entry_hi & PAGE_FRAME))) {
                        break;
                }
                ptr = ptr->next;
        }

        return ptr;
}

/* all functions that call this have to use the hpt_lock
 * to ensure mutex on the hpt. */
static bool insert_page_table_entry(struct addrspace *as,
                                    uint32_t entry_hi,
                                    uint32_t entry_lo) {

        uint32_t vpn = entry_hi & PAGE_FRAME;
        int index = hpt_hash(as, vpn);

        struct hpt_entry * new = kmalloc(sizeof(struct hpt_entry));
        new->pid = (uint32_t) as;
        new->entry_hi = vpn;
        new->entry_lo = entry_lo;
        new->next = NULL;

        if (hpt[index] == NULL) {
                hpt[index] = new;
                return true;
        }

        struct hpt_entry *ptr = hpt[index];
        while (ptr->next != NULL) {
                ptr = ptr->next;
        }
        ptr->next = new;

        return true;
}



static int define_memory(struct addrspace *as, vaddr_t addr,
                         size_t memsize, int permissions) {

        if (addr + memsize > MIPS_KSEG0) {
                return EFAULT;
        }

        uint32_t top = (addr + memsize + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t base = addr / PAGE_SIZE;
        paddr_t paddr = 0;

        for (uint32_t start = base; start < top; start++) {

                uint32_t entry_hi = start << FLAG_OFFSET;
                uint32_t entry_lo = (paddr & PAGE_FRAME) |
                                    (1 << HPTABLE_VALID) |
                                    (1 << HPTABLE_GLOBAL);

                if (permissions & HPTABLE_WRITE) {
                        entry_lo |= (1 << HPTABLE_DIRTY);
                } else {
                        entry_lo &= ~(1 << HPTABLE_DIRTY);
                }

                entry_lo |= permissions;
                entry_lo |= HPTABLE_SWRITE;

                spinlock_acquire(&hpt_lock);
                if (!insert_page_table_entry(as, entry_hi, entry_lo)) {

                        spinlock_release(&hpt_lock);
                        return ENOMEM;
                }

                spinlock_release(&hpt_lock);
        }

        return 0;
}



/* allocates a frame for a corresponding hash page table entry.
 * functions that call this have to use the hpt_lock */
int allocate_memory(struct hpt_entry * ptr) {
        vaddr_t vaddr = alloc_kpages(1);

        ptr->entry_lo |= vaddr;
        if (vaddr == 0) {
                return ENOMEM;
        }
        return 0;
}



/* flushes the TLB */
static void tlb_flush(void) {
        int i, spl;
        spl = splhigh();

        for (i = 0; i < NUM_TLB; i++) {
                tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
        }

        splx(spl);
 }



/* create a new empty address space */
struct addrspace *as_create(void) {
        struct addrspace *as;

        as = kmalloc(sizeof(struct addrspace));
        if (as == NULL) {
                return NULL;
        }
        tlb_flush();
        return as;
}



/* create an address space that is the exact copy
 * of the old one */
int as_copy(struct addrspace *old, struct addrspace **ret) {
        struct addrspace *newas;

        newas = as_create();
        if (newas == NULL) {
                return ENOMEM;
        }

        uint32_t pid = (uint32_t) old;

        spinlock_acquire(&hpt_lock);
        for (int i = 0; i < hpt_size; i++) {

                if (hpt[i] == NULL) {
                        continue;
                }

                struct hpt_entry *ptr = hpt[i];
                while (ptr != NULL) {
                        if (ptr->pid != pid) {
                                ptr = ptr->next;
                                continue;
                        }

                        vaddr_t old_entry_lo = ptr->entry_lo & PAGE_FRAME;
                        vaddr_t new_entry_lo = old_entry_lo;

                        if (old_entry_lo != 0) {
                                new_entry_lo = alloc_kpages(1);
                                if (new_entry_lo == 0) {
                                        spinlock_release(&hpt_lock);
                                        return ENOMEM;
                                }
                                memmove((void *) new_entry_lo,
                                        (void *) old_entry_lo,
                                        PAGE_SIZE);
                        }

                        new_entry_lo |= ((ptr->entry_lo & HPTABLE_STATEBITS) |
                                 (ptr->entry_lo & (1 << HPTABLE_DIRTY)) |
                                 (ptr->entry_lo & (1 << HPTABLE_VALID)) |
                                 (ptr->entry_lo & (1 << HPTABLE_GLOBAL)));

                        if (!insert_page_table_entry(newas, ptr->entry_hi,
                                                     new_entry_lo)) {

                                spinlock_release(&hpt_lock);
                                as_destroy(newas);
                                return ENOMEM;
                        }

                }
        }
        spinlock_release(&hpt_lock);

        *ret = newas;
        return 0;
}



/* dispose of an address space */
void as_destroy(struct addrspace *as) {
        uint32_t pid = (uint32_t) as;
        spinlock_acquire(&hpt_lock);

        for (int i = 0; i < hpt_size; i++) {

                if (hpt[i] == NULL) {
                        continue;
                }

                struct hpt_entry * ptr = hpt[i];
                struct hpt_entry * prev_ptr = NULL;
                while (ptr != NULL) {
                        if (ptr->pid != pid) {
                                prev_ptr = ptr;
                                ptr = ptr->next;
                                continue;
                        }
                        free_kpages(ptr->entry_lo & PAGE_FRAME);
                        struct hpt_entry * temp = ptr->next;
                        if (prev_ptr == NULL) {
                                hpt[i] = temp;
                        } else {
                                prev_ptr->next = temp;
                        }
                        kfree(ptr);
                        ptr = temp;
                }
        }

        spinlock_release(&hpt_lock);
        tlb_flush();
        kfree(as);
}



/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                     int readable, int writeable, int executable) {

        int result = define_memory(as, vaddr, memsize,
                                   (readable|writeable|executable) << 1);

        if (result) {
                return ENOMEM;
        }

        return 0;
}



/*
 * as_prepare_load does not need to do anything, because
 * as_define_region previously called the static function
 * "int define_memory", which set an SWRITE bit within each entry_lo.
 * 
 * When VM Fault is triggered, if it sees an
 * SWRITE bit within entry_lo, it will ignore the permissions bits
 * stored and instead temporarily allow read/write.
 * 
 * The SWRITE bit is cleared by as_complete_load.
 */
int as_prepare_load(struct addrspace *as) {
        (void) as;
        return 0;
}



int as_complete_load(struct addrspace *as) {

        uint32_t pid = (uint32_t) as;
        spinlock_acquire(&hpt_lock);

        for (int i = 0; i < hpt_size; i++) {

                if (hpt[i] == NULL) {
                        continue;
                }

                struct hpt_entry *ptr = hpt[i];
                while (ptr != NULL) {
                        if (ptr->pid == pid) {
                                ptr->entry_lo &= ~HPTABLE_SWRITE;
                        }
                        ptr = ptr->next;
                }

        }
        spinlock_release(&hpt_lock);
        /*
         * need to flush tlb because during prepare load we set
         * the SWRITE which consequently caused
         * the tlb entry to have dirty bit set,
         * but this SWRITE is only temporary so by flushing the tlb
         * the next time there won't be an SWRITE and
         * therefore the permission will be set to whatever is stored
         * within entry_lo.
         */
        tlb_flush();
        return 0;
}



int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {

        *stackptr = USERSTACK;
        vaddr_t location = USERSTACK - (PAGE_SIZE * STACK_PAGE);

        int result = define_memory(as, location, PAGE_SIZE * STACK_PAGE,
                                   HPTABLE_STACK_RW << 1);
        if (result) {
                return ENOMEM;
        }
        return 0;
}



void as_activate(void) {
        struct addrspace *as;

        as = proc_getas();
        if (as == NULL) {
                return;
        }

        tlb_flush();
}



void as_deactivate(void) {
        struct addrspace *as;

        as = proc_getas();
        if (as == NULL) {
                return;
        }

        tlb_flush();
}