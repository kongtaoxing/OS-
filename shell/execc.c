#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
int main()
{
    //ls -l
    //execl("/bin/ls","ls","-l",NULL);
    //execlp("ls","ls","-l",NULL);

    char *cmd[]={"ls","-l",NULL};
    //execv("/bin/ls",cmd);
    //execvp("ls",cmd);

    char *cmd_path[]={"wbyq=666","abcd=888",NULL};
    execle("/bin/ls","ls","-l",NULL,cmd_path);

    printf("exec函数执行失败.\n");
    return 0;
}

