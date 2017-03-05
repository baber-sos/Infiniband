#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h> /* Needed for KERN_INFO */
#include <linux/mm_types.h>
#include <asm/page.h>
#include <linux/page-flags.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/pid.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>




int syscallno; 
MODULE_LICENSE("GPL");
int pid, pid2;
struct task_struct *tsk, *tsk2;
struct mm_struct *mm, *mm2; 
module_param(pid, int, S_IRUSR);
module_param(pid2, int, S_IRUSR);

int lol_fault(struct vm_area_struct *vma, struct vm_fault *vmf){
	int tmp;
	printk(KERN_INFO "1.\n");
	printk(KERN_INFO "%u\n", VM_FAULT_MAJOR);
	printk(KERN_INFO "%u\n", VM_FAULT_LOCKED);
	printk(KERN_INFO "%u\n", VM_FAULT_RETRY);
	tmp = filemap_fault(vma, vmf);
	printk(KERN_INFO "%d\n", tmp);
	printk(KERN_INFO "%d\n", tmp&VM_FAULT_RETRY);
	printk(KERN_INFO "%d\n", tmp&VM_FAULT_LOCKED);
	return tmp;

}


struct vm_operations_struct file_ops = {
	.fault = lol_fault,
	.map_pages = filemap_map_pages,
	.page_mkwrite = filemap_page_mkwrite,
	// .remap_p/ages = generic_file_remap_pages,
};
	


int init_module(void){

	struct page *newPage, *newPage2;
	pte_t *pte, *pte2;
	struct vm_area_struct *vma, *vma2;
	void *page_addr, *page_addr2;
	char *arr, *arr2;
	unsigned long i, i2, j;
	char* old_content = kmalloc(sizeof(char)*4096, GFP_KERNEL);

	tsk = pid_task(find_vpid(pid), PIDTYPE_PID);
	tsk2 = pid_task(find_vpid(pid2), PIDTYPE_PID);

	mm = tsk->mm;
	mm2 = tsk2->mm;

	vma = find_vma(mm, mm->start_brk);
	vma2 = find_vma(mm2, mm2->start_brk);

	i = mm->start_brk;
	i2 = mm2->start_brk;

	printk(KERN_INFO "Heap One: %lx %lx \n", mm->start_brk, mm->brk);
	printk(KERN_INFO "Heap Two: %lx %lx \n", mm2->start_brk, mm2->brk);


	spin_lock(&mm->page_table_lock);
	spin_lock(&mm2->page_table_lock);	
	vma->vm_ops = &file_ops;
	
	for(i = mm->start_brk, i2 = mm2->start_brk; i != mm->brk && i2 != mm2->brk; i += PAGE_SIZE, i2 += PAGE_SIZE){
		pte = pte_offset_map(pmd_offset(pud_offset(pgd_offset(mm, i), i), i), i);
		pte2 = pte_offset_map(pmd_offset(pud_offset(pgd_offset(mm2, i2), i2), i2), i2);
		if(pte != NULL && pte2 != NULL){
			if(pte_present(*pte) && pte_present(*pte2)){
				
				printk("PTE One is %p \n", (void*)pte);
				printk("PTE Two is %p \n", (void*)pte2);

				newPage = pte_page(*pte);
				newPage2 = pte_page(*pte2);
				printk(KERN_INFO "The result of page#1 is: %d",PageAnon(newPage));
				printk(KERN_INFO "The result of page#2 is: %d",PageAnon(newPage2));

				if (PageAnon(newPage)){
					struct anon_vma *anon_vma;
					pgoff_t pgoff;
					struct anon_vma_chain *avc;

					anon_vma = page_lock_anon_vma_read(newPage);
					if (!anon_vma)
						return -1;
					pgoff = page_to_pgoff(newPage);
					int counter = 1;
					anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
						struct vm_area_struct *vma = avc->vma;
						printk("Number of VMAs are: %d\n", counter);
						counter++;

					}
					page_unlock_anon_vma_read(anon_vma);

				} else if (!PageKsm(newPage)) {
					struct address_mapping *mapping = newPage->mapping;
					pgoff_t pgoff;
					struct vm_area_struct *vma;

					VM_BUG_ON_PAGE(!PageLocked(newPage), newPage);

					if (!mapping)
					return -1;

					pgoff = page_to_pgoff(newPage);
					i_mmap_lock_read(mapping);
					int counter = 1;
					vma_interval_tree_foreach(vma, &mapping->i_mmap, pgoff, pgoff) {
				
						printk("Counter for mile mapped pages: %d\n", counter);
						counter++;
					}
					i_mmap_unlock_read(mapping);
				}

				page_addr = kmap(newPage);
				page_addr2 = kmap(newPage2);

				arr = (char*) page_addr;
				arr2 = (char*) page_addr2;
				
				for (j = 0; j < 4096; ++j)
				{
					old_content[j] = arr2[j];
					arr2[j] = arr[j];
					arr[j] = old_content[j];
				}

				kunmap(newPage);
				kunmap(newPage2);


			}
		}
	}
	kfree(old_content);
	spin_unlock(&mm2->page_table_lock);
	spin_unlock(&mm->page_table_lock);
	return 0;
}


void cleanup_module(void){
}

