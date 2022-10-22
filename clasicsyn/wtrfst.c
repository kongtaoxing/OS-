/*
* 	写者优先
*/

# include <stdio.h>
# include <stdlib.h>
# include <time.h>
# include <sys/types.h>
# include <pthread.h>
# include <semaphore.h>
# include <string.h>
# include <unistd.h>

sem_t RWMutex, mutex1, mutex2, mutex3, wrt;
int writeCount, readCount;
volatile int nLoopR = 0;
volatile int nLoopW = 0;


void* Reader(void* param) {
    long threadid = (long)param;
    while (nLoopR < 100){
        sem_wait(&mutex3);
        sem_wait(&RWMutex);
        sem_wait(&mutex2);
        readCount++;
        if(readCount == 1)
            sem_wait(&wrt);
        sem_post(&mutex2);
        sem_post(&RWMutex);
        sem_post(&mutex3);
        sleep(0.6);
        printf("Thread %ld: is reading\n", threadid);
        sem_wait(&mutex2);
        readCount--;
        if(readCount == 0)
            sem_post(&wrt);
        sem_post(&mutex2);
        nLoopR++;
    }
}

void* Writer(void* param) {
    long threadid = (long)param;
    while (nLoopW < 100){
        sem_wait(&mutex1);
        writeCount++;
        if(writeCount == 1){
            sem_wait(&RWMutex);
        }
        sem_post(&mutex1);
        sem_wait(&wrt);
        sleep(1);
        printf("Thread %ld: is writing\n", threadid );
        sem_post(&wrt);
        sem_wait(&mutex1);
        writeCount--;
        if(writeCount == 0) {
            sem_post(&RWMutex);
        }
        sem_post(&mutex1);
        nLoopW++;
    }
}

int main() {
    sem_init(&mutex1, 0, 1);
    sem_init(&mutex2, 0, 1);
    sem_init(&mutex3, 0, 1);
    sem_init(&wrt, 0, 1);
    sem_init(&RWMutex, 0, 1);

    readCount = writeCount = 0;
    pthread_t tid[4];
    pthread_create(&tid[0], NULL ,Reader, (void*)0);
    pthread_create(&tid[1], NULL ,Reader, (void*)1);
    pthread_create(&tid[2], NULL ,Writer, (void*)2);
    pthread_create(&tid[3], NULL, Writer, (void*)3);
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    sem_destroy(&mutex1);
    sem_destroy(&mutex2);
    sem_destroy(&mutex3);
    sem_destroy(&RWMutex);
    sem_destroy(&wrt);
    return 0;
}
