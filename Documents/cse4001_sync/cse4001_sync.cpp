// cse4001_sync.cpp
// Compile: make
// Usage: ./cse4001_sync <problem#>
// 1 = No-starve readers-writers (5 readers, 5 writers)
// 2 = Writer-priority readers-writers (5 readers, 5 writers)
// 3 = Dining philosophers #1 (limit seating to 4)
// 4 = Dining philosophers #2 (odd-left / even-right fork pick ordering)

#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <semaphore.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std;

/* Simple Semaphore wrapper using POSIX semaphores */
class Semaphore {
public:
    Semaphore(int initialValue = 0) {
        sem_init(&mSemaphore, 0, initialValue);
    }
    ~Semaphore() {
        sem_destroy(&mSemaphore);
    }
    void wait() { sem_wait(&mSemaphore); }
    void signal() { sem_post(&mSemaphore); }
private:
    sem_t mSemaphore;
};

/* Common utilities */
void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* ---------- Problem 1: No-starve readers-writers ----------
   Implementation uses a 'turnstile' to serialize arrivals (so writers
   can't be continually overtaken by new readers) and a readers 'lightswitch'
   to manage the reader count. This is essentially a fair solution.
*/
namespace NoStarveRW {
    const int NREADERS = 5;
    const int NWRITERS = 5;

    Semaphore turnstile(1);    // forces arriving threads to queue; helps fairness
    Semaphore roomEmpty(1);    // 1 if no writer; writers take it to write
    pthread_mutex_t readersMutex = PTHREAD_MUTEX_INITIALIZER;
    int readersCount = 0;

    void *reader(void *id) {
        long me = (long)id;
        for (int i=0;i<5;i++) {
            turnstile.wait();        // wait in line so writers can't be starved
            turnstile.signal();

            pthread_mutex_lock(&readersMutex);
            readersCount++;
            if (readersCount == 1) roomEmpty.wait(); // first reader locks out writers
            pthread_mutex_unlock(&readersMutex);

            printf("Reader %ld: reading (iteration %d)\n", me, i+1);
            fflush(stdout);
            msleep(80 + (me*10));

            pthread_mutex_lock(&readersMutex);
            readersCount--;
            if (readersCount == 0) roomEmpty.signal(); // last reader leaves
            pthread_mutex_unlock(&readersMutex);

            msleep(50);
        }
        return nullptr;
    }

    void *writer(void *id) {
        long me = (long)id;
        for (int i=0;i<5;i++) {
            turnstile.wait();     // writer blocks new arrivals so it can get in
            roomEmpty.wait();     // ensure exclusive access to room
            // now allow next arrivals after we take roomEmpty
            turnstile.signal();

            printf("Writer %ld: writing (iteration %d)\n", me, i+1);
            fflush(stdout);
            msleep(120 + (me*20));

            roomEmpty.signal();
            msleep(80);
        }
        return nullptr;
    }

    void run() {
        vector<pthread_t> rthreads(NREADERS), wthreads(NWRITERS);
        for (long i=0;i<NREADERS;i++) pthread_create(&rthreads[i], nullptr, reader, (void*)(i+1));
        for (long i=0;i<NWRITERS;i++) pthread_create(&wthreads[i], nullptr, writer, (void*)(i+1));
        for (int i=0;i<NREADERS;i++) pthread_join(rthreads[i], nullptr);
        for (int i=0;i<NWRITERS;i++) pthread_join(wthreads[i], nullptr);
    }
}

/* ---------- Problem 2: Writer-priority readers-writers ----------
   This pattern gives writers priority: when a writer arrives it prevents new readers
   from entering. Implemented using a writer 'lightswitch' + a readers lightswitch.
*/
namespace WriterPriorityRW {
    const int NREADERS = 5;
    const int NWRITERS = 5;

    Semaphore roomEmpty(1);         // protects shared resource
    pthread_mutex_t readerMutex = PTHREAD_MUTEX_INITIALIZER;
    int readersCount = 0;

    pthread_mutex_t writerMutex = PTHREAD_MUTEX_INITIALIZER;
    int writersCount = 0;
    Semaphore readersQueue(1);      // readers must acquire this when writers are present

    void *reader(void *id) {
        long me = (long)id;
        for (int i=0;i<5;i++) {
            readersQueue.wait();            // block when writers want priority
            readersQueue.signal();

            pthread_mutex_lock(&readerMutex);
            readersCount++;
            if (readersCount == 1) roomEmpty.wait();
            pthread_mutex_unlock(&readerMutex);

            printf("Reader %ld: reading (iteration %d)\n", me, i+1);
            fflush(stdout);
            msleep(70 + me*10);

            pthread_mutex_lock(&readerMutex);
            readersCount--;
            if (readersCount == 0) roomEmpty.signal();
            pthread_mutex_unlock(&readerMutex);

            msleep(40);
        }
        return nullptr;
    }

    void *writer(void *id) {
        long me = (long)id;
        for (int i=0;i<5;i++) {
            pthread_mutex_lock(&writerMutex);
            writersCount++;
            if (writersCount == 1) readersQueue.wait(); // first writer blocks readers
            pthread_mutex_unlock(&writerMutex);

            roomEmpty.wait();   // exclusive access
            printf("Writer %ld: writing (iteration %d)\n", me, i+1);
            fflush(stdout);
            msleep(110 + me*15);
            roomEmpty.signal();

            pthread_mutex_lock(&writerMutex);
            writersCount--;
            if (writersCount == 0) readersQueue.signal(); // last writer allows readers
            pthread_mutex_unlock(&writerMutex);

            msleep(60);
        }
        return nullptr;
    }

    void run() {
        vector<pthread_t> rthreads(NREADERS), wthreads(NWRITERS);
        for (long i=0;i<NREADERS;i++) pthread_create(&rthreads[i], nullptr, reader, (void*)(i+1));
        for (long i=0;i<NWRITERS;i++) pthread_create(&wthreads[i], nullptr, writer, (void*)(i+1));
        for (int i=0;i<NREADERS;i++) pthread_join(rthreads[i], nullptr);
        for (int i=0;i<NWRITERS;i++) pthread_join(wthreads[i], nullptr);
    }
}

/* ---------- Problem 3: Dining Philosophers #1 ----------
   Prevent deadlock by allowing at most N-1 philosophers to sit at the table concurrently.
   Here N=5, so use a 'room' semaphore initialized to 4.
*/
namespace Dining1 {
    const int N = 5;
    Semaphore forks[N] = {Semaphore(1),Semaphore(1),Semaphore(1),Semaphore(1),Semaphore(1)};
    Semaphore room(4); // allow at most 4 philosophers to try to pick forks

    void *philosopher(void *id) {
        long me = (long)id; // 0..4
        int left = me;
        int right = (me+1) % N;
        for (int i=0;i<6;i++) {
            printf("Philosopher %ld: Thinking\n", me+1);
            fflush(stdout);
            msleep(50 + (me*10));

            room.wait(); // enter table (ensures at least one will be able to pick both forks)
            forks[left].wait();
            forks[right].wait();

            printf("Philosopher %ld: Eating (iteration %d)\n", me+1, i+1);
            fflush(stdout);
            msleep(90 + (me*10));

            forks[right].signal();
            forks[left].signal();
            room.signal();

            msleep(40);
        }
        return nullptr;
    }

    void run() {
        vector<pthread_t> threads(N);
        for (long i=0;i<N;i++) pthread_create(&threads[i], nullptr, philosopher, (void*)i);
        for (int i=0;i<N;i++) pthread_join(threads[i], nullptr);
    }
}

/* ---------- Problem 4: Dining Philosophers #2 ----------
   Prevent deadlock by having one philosopher pick forks in reversed order.
   Implementation: odd-index philosophers pick left then right, even-index pick right then left.
*/
namespace Dining2 {
    const int N = 5;
    Semaphore forks[N] = {Semaphore(1),Semaphore(1),Semaphore(1),Semaphore(1),Semaphore(1)};

    void *philosopher(void *id) {
        long me = (long)id; // 0..4
        int left = me;
        int right = (me+1) % N;
        for (int i=0;i<6;i++) {
            printf("Philosopher %ld: Thinking\n", me+1);
            fflush(stdout);
            msleep(60 + me*10);

            if ((me % 2) == 0) {
                // even philosopher picks right first then left
                forks[right].wait();
                forks[left].wait();
            } else {
                // odd philosopher picks left first then right
                forks[left].wait();
                forks[right].wait();
            }

            printf("Philosopher %ld: Eating (iteration %d)\n", me+1, i+1);
            fflush(stdout);
            msleep(100 + me*5);

            // release
            forks[left].signal();
            forks[right].signal();

            msleep(50);
        }
        return nullptr;
    }

    void run() {
        vector<pthread_t> threads(N);
        for (long i=0;i<N;i++) pthread_create(&threads[i], nullptr, philosopher, (void*)i);
        for (int i=0;i<N;i++) pthread_join(threads[i], nullptr);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <problem#>\n1=No-starve RW\n2=Writer-priority RW\n3=Dining #1\n4=Dining #2\n", argv[0]);
        return 1;
    }
    int prob = atoi(argv[1]);
    printf("Starting problem %d\n", prob);
    fflush(stdout);

    switch (prob) {
        case 1:
            NoStarveRW::run();
            break;
        case 2:
            WriterPriorityRW::run();
            break;
        case 3:
            Dining1::run();
            break;
        case 4:
            Dining2::run();
            break;
        default:
            fprintf(stderr, "Invalid problem number. Choose 1..4\n");
            return 1;
    }
    printf("Problem %d finished\n", prob);
    return 0;
}
