/* ----------------------------------------------------------------- */
/* PROGRAM fork-02.c                                                 */
/*   This program runs two processes, a parent and a child.  Both of */
/* them run the same loop printing some messages.  Note that printf()*/
/* is used in this program.                                          */
/* ----------------------------------------------------------------- */

#include  <stdio.h>
#include  <sys/types.h>
#include  <unistd.h>
#include  <stdlib.h>


#define   MAX_COUNT  200

void  ChildProcess(void);                /* child process prototype  */
void  ParentProcess(void);               /* parent process prototype */

void  main(void)
{
     pid_t  pid;
     int i = 0;
     // char* strvar;// =(char *) malloc(5000);
     int *a = malloc(10000);
     for (i = 0; i < 10000; ++i)
     {
          a[i] = i;
     }
     pid = fork();

     if (pid == 0) 
          ChildProcess();
     else 
          ParentProcess();
}

void  ChildProcess(void)
{
     printf("Child process: %d.\n",getpid());
     // fgets (strvar, 100, stdin);
     sleep(100);

}

void  ParentProcess(void)
{
     printf("Parent process: %d.\n",getpid());
     // fgets (strvar, 100, stdin);
     sleep(100);

}
