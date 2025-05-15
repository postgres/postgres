#include "c.h"

#include "port/memory_access_tracing.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wreserved-identifier"

#define NO_SANITIZE __attribute__((no_sanitize("all")))


int ENABLE_TRACE = 1;
static int trace_fd = -1;

int memory_access_counter = 0;

NO_SANITIZE
__attribute__((constructor))
static void init_trace() {
    pid_t pid = getpid();
    char buffer[128];
    snprintf(buffer, 128, "/tmp/postgres_%d.log", pid);
    trace_fd = open(buffer, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ENABLE_TRACE = 1;
}

NO_SANITIZE
void enableMemoryAccessesCoverage() {
    pid_t pid = getpid();
    char buffer[128];
    snprintf(buffer, 128, "/tmp/postgres_%d.log", pid);
    trace_fd = open(buffer, O_WRONLY | O_CREAT | O_APPEND, 0644);
    ENABLE_TRACE = 1;
}

NO_SANITIZE
void disableMemoryAccessesCoverage() {
    ENABLE_TRACE = 0;
    if (trace_fd != -1) {
        close(trace_fd);
        trace_fd = -1;
    }
}

NO_SANITIZE
int getMemoryAccessCount()
{
    return memory_access_counter;
}

NO_SANITIZE
void resetMemoryAccessCount()
{
    memory_access_counter = 0;
}


NO_SANITIZE
void __sanitizer_cov_trace_pc()
{
    // Just need this
}

NO_SANITIZE
void __sanitizer_cov_load1(uint8* addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "l1\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_load2(uint16 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "l2\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_load4(uint32 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "l4\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_load8(uint64 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "l8\t%lu\t%lu\t%lu\n",
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_load16(__int128 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "l16\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_store1(uint8 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "s1\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_store2(uint16 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "s2\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_store4(uint32 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "s4\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_store8(uint64 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "s8\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}

NO_SANITIZE
void __sanitizer_cov_store16(__int128 * addr)
{
    if (ENABLE_TRACE == 1) {
        ++memory_access_counter;
        char buffer[128];
        int len = snprintf(buffer, sizeof(buffer), "s16\t%lu\t%lu\t%lu\n", 
                        addr, pthread_self(), __builtin_return_address(0));
        write(trace_fd, buffer, len);
    }
}
