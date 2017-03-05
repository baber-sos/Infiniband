#include <sys/types.h> 
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>


int main(int argc, char *argv[]){

	printf("PID: %d\n", getpid());
	int* a = malloc(sizeof(int)); 
	int* b = malloc(sizeof(int));
	*a = atoi(argv[1]);
	*b = atoi(argv[1]); 
	char strvar[100];
	fgets (strvar, 100, stdin);
	printf("The result of %d is %d\n", getpid(), *a + *b);
	
	return 0;
}
