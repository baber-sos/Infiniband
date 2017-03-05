#include <stdlib.h>
#include <stdio.h>
unsigned long **myPtes;
int numberOfArrays = 5;
int maxSizeArray = 100;
int currentArray = 0;
int currentIndex = -1;

void allocateArray(){
	int i;
	myPtes = malloc(sizeof(unsigned long*)*numberOfArrays);
	for (i = 0; i < numberOfArrays; ++i)
	{
		myPtes[i] = malloc(sizeof(unsigned long)*maxSizeArray);
	}
}
void freeArray(){
	int i;
	for (i = 0; i < numberOfArrays; ++i)
	{
		free(myPtes[i]);
	}
	free(myPtes);
}

void addInArray(unsigned long element){
	if (currentArray < numberOfArrays){
		if (currentIndex < maxSizeArray-1){
			currentIndex += 1;
		}
		else{
			currentArray += 1;
			currentIndex = 0;
		}
		myPtes[currentArray][currentIndex] = element;
	}
}

void printArray(){
	int i,j;
	printf("The contents of the array are:\n");
	for (i = 0; i < numberOfArrays; ++i)
	{
		for (j = 0; j < maxSizeArray; ++j)
		{
			printf("%lu, ", myPtes[i][j]);
		}
		printf("\n");
	}
	printf("\n");
}

void fillArray(){
	int i,j, res;
	for (i = 0; i < numberOfArrays; ++i)
	{
		for (int j = 0; j < maxSizeArray; ++j)
		{
			int res = i+j;
			addInArray(res);
		}
	}
}

int main(){
	allocateArray();
	fillArray();
	printArray();
	freeArray();
	return 0;
}