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
#include <linux/swap.h>

extern unsigned long **myPtes_IN;
extern unsigned long **myPtes_OUT;
extern int currentArray;
extern int currentIndex;

struct pageContentStore{

	unsigned long value;
	char content[4096];

};


extern struct pageContentStore* pageContentStore_OUT;
extern struct pageContentStore* pageContentStore_IN;
extern int pageFilled_IN;
MODULE_LICENSE("GPL");
int maxArrays = 5;
int maxElements = 20000;


int lol_fault(struct vm_area_struct *vma, struct vm_fault *vmf){
	int tmp;
	tmp = filemap_fault(vma, vmf);
	return tmp;

}


struct vm_operations_struct file_ops = {
	.fault = lol_fault,
	.map_pages = filemap_map_pages,
	.page_mkwrite = filemap_page_mkwrite,
	// .remap_p/ages = generic_file_remap_pages,
};
	

void printArray(unsigned long** inp){
	int i,j;
	if (!inp)
		return;
	printk(KERN_INFO"The contents of the array are:\n");
	for (i = 0; i < maxArrays; ++i)
	{
		for (j = 0; j < maxElements; ++j)
		{
			printk(KERN_INFO"%lu, ", inp[i][j]);
		}
		printk(KERN_INFO"\n");
	}
	printk( KERN_INFO"\n");
}

int compTwoArrays(unsigned long** array1, unsigned long** array2){
	int i,j, x, y;
	int similarity = 0;
	for (i = 0; i <= currentArray_IN; ++i)
	{
		for (j = 0; j < maxElements; ++j)
		{
			if (i == currentArray_IN && j > currentIndex_IN)
				goto finish;
			for (x = 0; x <= currentArray; ++x)
			{
				for (y = 0; y < maxElements; ++y)
				{	if (x == currentArray && y > 						currentIndex)
						goto nextIter;
					if(array1[i][j] == array2[x][y]){
						similarity++;
						goto nextIter;

					}
						
				}
			}
nextIter:
;
		}
		printk("One element done\n");
	}
;
finish:
	return similarity;
}


int init_module(void){
	int x = 0;
	int i,j,k;
	int decision = 1;
	if (myPtes_IN && myPtes_OUT){
		x = compTwoArrays(myPtes_IN, myPtes_OUT);
		printk("The number of same elements are: %d.\n", x);
		printk("currentArray_IN %d.\n currentIndex_IN %d.\n", 			currentArray_IN, currentIndex_IN);
		printk("currentArray_OUT %d.\n currentIndex_OUT %d.\n", 		currentArray, currentIndex);	
	}
	// printArray(myPtes_OUT);
	for (i = 0; i < pageFilled_IN; ++i){
		decision = 1;
		for(k = 0; k < 1000; k++){
			if (pageContentStore_OUT[k].value == pageContentStore_IN[i].value){
				printk(KERN_INFO "Matching swap table entries found %lu, %lu.\n", pageContentStore_OUT[k].value , pageContentStore_IN[i].value);
				for (j = 0; j < 4096; ++j)
				{
					if(pageContentStore_OUT[k].content[j] != pageContentStore_IN[i].content[j])
						decision = 0;
				}
				if (decision == 1){
					printk(KERN_INFO "Page contents match.\n\n");
					goto tag1;
				}
			}
		}
	}
tag1:
	printk("\nEND.\n");
	return 0;
	
}


void cleanup_module(void){
}

