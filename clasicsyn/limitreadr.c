# include <stdio.h>
# include <stdlib.h>
# include <pthread.h>
# include <semaphore.h>
# include <string.h>
# include <unistd.h>

sem_t wrt, mutex, readerNum;

int readCount;

void* Reader(void* param) {
    long threadid = (long)param;
    while (1){
        sem_wait(&readerNum);
        sem_wait(&mutex);
        readCount++;
        if(readCount == 1)
            sem_wait(&wrt);
        sem_post(&mutex);
        printf("Thread %ld: is reading\n", threadid);
        sleep(3);
        sem_wait(&mutex);
        readCount--;
        if(readCount == 0)
            sem_post(&wrt);
        sem_post(&mutex);
        sem_post(&readerNum);
    }
}

void* Writer(void* param) {
    long threadid = (long)param;
    while (1){
        sem_wait(&wrt);
        printf("Thread %ld: is writing\n", threadid);
        sleep(5);
        sem_post(&wrt);
    }
}


int main() {
    //初始化信号量
    //读者数量限定为5
    sem_init(&readerNum,0,5);
    sem_init(&mutex, 0, 1);
    sem_init(&wrt, 0, 1);
    readCount = 0;
    pthread_t tid[10];
    //创建6个读者线程，观察第六个读者能否运行
    pthread_create(&tid[0], NULL ,Reader, (void*)0);
    pthread_create(&tid[1], NULL ,Reader, (void*)1);
    pthread_create(&tid[2], NULL ,Reader, (void*)2);
    pthread_create(&tid[3], NULL ,Reader, (void*)3);
    pthread_create(&tid[4], NULL ,Reader, (void*)4);
    pthread_create(&tid[5], NULL ,Reader, (void*)5);
    pthread_create(&tid[7], NULL, Writer, (void*)6);
    pthread_create(&tid[8], NULL, Writer, (void*)7);
    int c=0;
    while (1){
        c = getchar();
        if (c=='q' || c=='Q'){
            for (int i = 0; i < 8; ++i) {
                pthread_cancel(tid[i]);
            }
            break;
        }
    }
    //信号量销毁
    sem_destroy(&mutex);
    sem_destroy(&wrt);
    return 0;
}

