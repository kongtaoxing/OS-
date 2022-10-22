#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(){
    int p1,p2;
    //由fork创建的新进程被称为子进程（ child process）。该函数被调用一次，但返回两次。
    //两次返回的区别是子进程的返回值是0，而父进程的返回值则是新子进程的进程ID
    while((p1=fork())==-1);
    if (p1==0)
        printf("Child1 \n");
    else
        {
          while((p2=fork())==-1);
          if (p2==0)
            printf("Child2 \n");
          else
            printf("Parent \n"); 
        }
    return 0;  
}
