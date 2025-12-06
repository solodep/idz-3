#define _XOPEN_SOURCE 700

#include "lib_common.hpp"

#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#include <mqueue.h>
#include <errno.h>

static void die(const char* msg) { perror(msg); std::exit(1); }

static int cmp_entry(const BookEntry& a, const BookEntry& b) {
    if (a.title != b.title) return a.title < b.title ? -1 : 1;
    return 0;
}

static void merge_catalog(SharedData* shm, const std::vector<BookEntry>& local) {
    std::vector<BookEntry> merged;
    merged.reserve(shm->catalog_count + (int)local.size());

    int i=0, j=0;
    while (i < shm->catalog_count && j < (int)local.size()) {
        if (cmp_entry(shm->catalog[i], local[j]) <= 0) merged.push_back(shm->catalog[i++]);
        else merged.push_back(local[j++]);
    }
    while (i < shm->catalog_count) merged.push_back(shm->catalog[i++]);
    while (j < (int)local.size()) merged.push_back(local[j++]);

    std::memcpy(shm->catalog, merged.data(), merged.size() * sizeof(BookEntry));
    shm->catalog_count = (int)merged.size();
}

// broadcast всем наблюдателям: у каждого своя mq
static void send_log(SharedData* shm, sem_t* sem_catalog, const std::string& msg) {
    char names[MAX_OBSERVERS][OBS_NAME_LEN]{};
    int count = 0;

    sem_wait(sem_catalog);
    count = shm->observers_count;
    for (int i=0;i<count;++i) std::strncpy(names[i], shm->observer_queues[i], OBS_NAME_LEN-1);
    sem_post(sem_catalog);

    for (int i=0;i<count;++i) {
        mqd_t q = mq_open(names[i], O_WRONLY | O_NONBLOCK);
        if (q == (mqd_t)-1) continue;
        if (mq_send(q, msg.c_str(), msg.size()+1, 0) == -1) {
            // EAGAIN если очередь переполнена — игнорируем, чтобы worker не стопорился
        }
        mq_close(q);
    }
}

int main(int argc, char** argv) {
    int wid = (argc>=2) ? std::atoi(argv[1]) : 0;

    // ждём пока init создаст shm и sem
    int fd = -1;
    for (;;) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd != -1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto* shm = (SharedData*)mmap(nullptr, sizeof(SharedData), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("mmap");
    close(fd);

    sem_t* sem_portfolio = SEM_FAILED;
    sem_t* sem_catalog   = SEM_FAILED;

    for (;;) {
        sem_portfolio = sem_open(SEM_PORTFOLIO_NAME, 0);
        sem_catalog   = sem_open(SEM_CATALOG_NAME, 0);
        if (sem_portfolio != SEM_FAILED && sem_catalog != SEM_FAILED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    while (!shm->initialized) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "[WORKER " << wid << "] started\n";

    while (true) {
        sem_wait(sem_portfolio);
        int row = shm->next_row;
        if (row >= shm->M_rows) {
            sem_post(sem_portfolio);
            std::cout << "[WORKER " << wid << "] no more rows, exit\n";
            send_log(shm, sem_catalog, "[WORKER " + std::to_string(wid) + "] exit");
            break;
        }
        shm->next_row++;
        sem_post(sem_portfolio);

        std::string m1 = "[WORKER " + std::to_string(wid) + "] got row " + std::to_string(row);
        std::cout << m1 << "\n";
        send_log(shm, sem_catalog, m1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));


        std::vector<BookEntry> local;
        local.reserve(shm->N_shelves * shm->K_per_shelf);

        for (int s=0; s<shm->N_shelves; ++s)
            for (int p=0; p<shm->K_per_shelf; ++p)
                local.push_back({shm->book_at_pos[row][s][p], row, s, p});

        std::sort(local.begin(), local.end(), [](const auto& a, const auto& b){ return a.title < b.title; });

        sem_wait(sem_catalog);
        merge_catalog(shm, local);
        shm->rows_done++;
        sem_post(sem_catalog);

        std::string m2 = "[WORKER " + std::to_string(wid) + "] finished row " + std::to_string(row);
        std::cout << m2 << "\n";
        send_log(shm, sem_catalog, m2);
    }

    sem_close(sem_portfolio);
    sem_close(sem_catalog);
    munmap(shm, sizeof(SharedData));
    return 0;
}
