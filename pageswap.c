#include "types.h"
#include "param.h"
#include "defs.h"
#include "mmu.h"
#include "memlayout.h"
#include "x86.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#define NSWAPSLOTS SWAPBLOCKS/8
struct
{
    struct spinlock lock;
    struct swapslot slots[NSWAPSLOTS];
} swapspace;


void swapinit()
{
    initlock(&swapspace.lock, "swaplock");
    for(int i=0;i<NSWAPSLOTS;i++)
    {
        swapspace.slots[i].is_free = 1;
        swapspace.slots[i].page_perm = 0;
        swapspace.slots[i].num = 2+8*i;
        for (int j=0; j<NPROC; j++)
            swapspace.slots[i].procs[j] = -1;
    }
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

pte_t*
get_pte(pde_t *pgdir, uint pa)
{
  pde_t *pde;
  pte_t *pgtab;

   for(int i = 0; i < NPDENTRIES; i++)
   {
       pde = &pgdir[i];
       if(*pde & PTE_P)
       {
            pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
            for(int j = 0; j < NPTENTRIES; j++)
            {
                if(pgtab[j]!=0 && PTE_ADDR(pgtab[j]) == pa)
                {
                    return &pgtab[j];
                }
            }
       }
   }
   panic("get_pte: no such page table entry");
}

void 
freeup_slot(int slot_idx)
{
    acquire(&swapspace.lock);
    swapspace.slots[slot_idx].is_free = 1;
    swapspace.slots[slot_idx].page_perm = 0;
    for (int i=0; i<NPROC; i++)
        swapspace.slots[slot_idx].procs[i] = -1;
    release(&swapspace.lock);
}

void
swapout()
{
    struct proc* victim = victim_process();
    // cprintf("Swapping out victim %d\n", victim->pid);
    pte_t* page = page_to_swap(victim);
    // cprintf("Fetched victim page\n");
    // victim->rss -= PGSIZE;

    acquire(&swapspace.lock);
    int i;
    for(i=NSWAPSLOTS-1;i>=0;i--)
    {
        if(swapspace.slots[i].is_free)
            break;
    }
    release(&swapspace.lock);
    // cprintf("SWAPOUT SLOT NO: %d\n", i);

    if(i == -1)
        panic("swapout: no free slots\n");

    swapspace.slots[i].is_free = 0;
    swapspace.slots[i].page_perm = PTE_FLAGS(*page);
    for (int j=0; j<NPROC; j++)
    {
        swapspace.slots[i].procs[j] = get_rmap_pid(PTE_ADDR(*page), j);
    }
    reset_rmap_entry(PTE_ADDR(*page));
    
    uint phy_addr = PTE_ADDR(*page);
    write_page_to_disk(ROOTDEV, (char*)P2V(phy_addr), swapspace.slots[i].num);
    kfree((char*)P2V(phy_addr));

    acquire(&swapspace.lock);
    for (int j=0; j<NPROC; j++)
    {
        if (swapspace.slots[i].procs[j] != -1)
        {
            struct proc *p;
            p = get_proc(swapspace.slots[i].procs[j]);
            p->rss -= PGSIZE;
            pte_t *pte;
            pte = get_pte(p->pgdir, phy_addr);
            *pte = (swapspace.slots[i].num << PTXSHIFT) | PTE_FLAGS(*pte);
            *pte &= ~PTE_P;
            *pte |= PTE_SW;
        }
    }
    release(&swapspace.lock);
}

void handle_swap_fault()
{
    struct proc* p = myproc();
    uint addr = rcr2();

    pte_t* pte = walkpgdir(p->pgdir, (char*)addr, 0);
    int block_no = PTE_ADDR(*pte) >> PTXSHIFT;

    char* new_page = kalloc();
    read_page_from_disk(ROOTDEV, new_page, block_no);

    int swap_slot_no = (block_no - 2)/8;
    // cprintf("BLOCK NO: %d\n", block_no);
    // cprintf("SWAPIN SLOT NO: %d\n", swap_slot_no);
    acquire(&swapspace.lock);
    for (int i=0; i<NPROC; i++)
    {
        if (swapspace.slots[swap_slot_no].procs[i] != -1)
        {
            // cprintf("pid: %d\n", swapspace.slots[swap_slot_no].procs[i]);
            p = get_proc(swapspace.slots[swap_slot_no].procs[i]);
            p->rss += PGSIZE;
            pte = walkpgdir(p->pgdir, (char*)addr, 0);
            *pte = V2P(new_page) | swapspace.slots[swap_slot_no].page_perm;
            *pte |= PTE_P;
            *pte &= ~PTE_SW;
            increment_rmap(PTE_ADDR(*pte), p->pid);
        }
    }
    release(&swapspace.lock);
    freeup_slot(swap_slot_no);
}