#include "Host.h"

#define WORKERS 4
#define ITERATIONS 200000

static long counter;
static int lock;
static long guarded;
static volatile long payload[2];
static volatile int ready;
static long observedMismatch;

#define SLOTS (1 << 20)
static volatile int slotData[SLOTS];
static volatile int slotFlag[SLOTS];

static void* SlotWriter(void* argument) {
    for (int i = 0; i < SLOTS; ++i) {
        slotData[i] = i + 1;
        slotFlag[i] = 1;
    }
    return argument;
}

static void* SlotReader(void* argument) {
    for (int i = 0; i < SLOTS; ++i) {
        while (!slotFlag[i]) {
        }
        if (slotData[i] != i + 1) observedMismatch++;
    }
    return argument;
}

static volatile int sweepDone;

static void* SlotSweeper(void* argument) {
    while (!sweepDone) {
        for (int i = 0; i < SLOTS; i += 16) {
            int flag = slotFlag[i];
            int data = slotData[i];
            if (flag && data != i + 1) observedMismatch++;
        }
    }
    return argument;
}

static void Lock(void) {
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock, __ATOMIC_RELAXED)) {
        }
    }
}

static void Unlock(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

static void* Worker(void* argument) {
    long id = (long)argument;
    for (int i = 0; i < ITERATIONS; ++i) {
        __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
        if ((i & 15) == 0) {
            Lock();
            guarded += id + 1;
            Unlock();
        }
    }
    return 0;
}

static void* Producer(void* argument) {
    for (long round = 1; round <= 20000; ++round) {
        while (ready) {
        }
        payload[0] = round;
        payload[1] = round * 3;
        ready = 1;
    }
    return argument;
}

static void* Consumer(void* argument) {
    for (long round = 1; round <= 20000; ++round) {
        while (!ready) {
        }
        if (payload[0] != round || payload[1] != round * 3) observedMismatch++;
        ready = 0;
    }
    return argument;
}

long test_main(void) {
    long handles[WORKERS];
    for (long i = 0; i < WORKERS; ++i) handles[i] = host_thread_create(Worker, (void*)i);
    for (int i = 0; i < WORKERS; ++i) host_thread_join(handles[i]);
    host_print_i64("counter", counter);
    host_print_i64("guarded", guarded);
    long producer = host_thread_create(Producer, 0);
    long consumer = host_thread_create(Consumer, 0);
    host_thread_join(producer);
    host_thread_join(consumer);
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < SLOTS; ++i) {
            slotFlag[i] = 0;
            slotData[i] = 0;
        }
        sweepDone = 0;
        long sweeper = host_thread_create(SlotSweeper, 0);
        long reader = host_thread_create(SlotReader, 0);
        long writer = host_thread_create(SlotWriter, 0);
        host_thread_join(writer);
        host_thread_join(reader);
        sweepDone = 1;
        host_thread_join(sweeper);
    }
    host_print_i64("mismatches", observedMismatch);
    return counter + guarded;
}
