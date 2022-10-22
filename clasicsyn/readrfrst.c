/*
* 读者优先
*/
# include <stdio.h>
# include <stdlib.h>
# include <pthread.h>
# include <semaphore.h>
# include <string.h>
# include <unistd.h>

sem_t wrt, mutex1, mutex2, RWMutex;
int readCount, writeCount;
volatile int nLoopR = 0;
volatile int nLoopW = 0;

void* Reader(void* param) {
    long threadid = (long)param;
    while (nLoopR < 100){
        sem_wait(&mutex1);
        readCount++;
        if(readCount == 1)
            sem_wait(&RWMutex);

        sem_post(&mutex1);
        sem_wait(&wrt);
        sleep(0.6);
        printf("Thread %ld: is reading\n", threadid);
        sem_post(&wrt);
        sem_wait(&mutex1);
        readCount--;
        if(readCount == 0)
            sem_post(&RWMutex);
        sem_post(&mutex1);
        nLoopR++;
    }
}

void* Writer(void* param) {
    long threadid = (long)param;
    while (nLoopW < 100){
        sem_wait(&mutex2);
        sem_wait(&RWMutex);
        writeCount++;
        if(writeCount == 1)
            sem_wait(&wrt);
        sem_post(&RWMutex);
        sem_post(&mutex2);
        sleep(1);
        printf("Thread %ld: is writing\n", threadid);
        sem_wait(&mutex2);
        writeCount--;
        if(writeCount == 0)
            sem_post(&wrt);
        sem_post(&mutex2);
        nLoopW++;
    }
}

int main() {
    sem_init(&mutex1, 0, 1);
    sem_init(&mutex2, 0, 1);
    sem_init(&wrt, 0, 1);
    sem_init(&RWMutex, 0, 1);

    readCount = writeCount = 0;
    pthread_t tid[4];
    pthread_create(&tid[0], NULL ,Writer, (void*)0);
    pthread_create(&tid[1], NULL ,Writer, (void*)1);
    pthread_create(&tid[2], NULL ,Reader, (void*)2);
    pthread_create(&tid[3], NULL, Reader, (void*)3);
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    sem_destroy(&mutex1);
    sem_destroy(&mutex2);
    sem_destroy(&RWMutex);
    sem_destroy(&wrt);
    return 0;
}
