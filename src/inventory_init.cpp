#define _XOPEN_SOURCE 700

#include "lib_common.hpp"

#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <cstring>
#include <csignal>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>

static SharedData* g_shm = nullptr;
static sem_t* g_sem_portfolio = SEM_FAILED;
static sem_t* g_sem_catalog   = SEM_FAILED;

static void die(const char* msg) { perror(msg); std::exit(1); }

static void cleanup() {
    if (g_sem_portfolio != SEM_FAILED) { sem_close(g_sem_portfolio); sem_unlink(SEM_PORTFOLIO_NAME); g_sem_portfolio = SEM_FAILED; }
    if (g_sem_catalog   != SEM_FAILED) { sem_close(g_sem_catalog);   sem_unlink(SEM_CATALOG_NAME);   g_sem_catalog   = SEM_FAILED; }

    if (g_shm) { munmap(g_shm, sizeof(SharedData)); shm_unlink(SHM_NAME); g_shm = nullptr; }
}

static void on_sigint(int) {
    std::cerr << "\n[INIT] SIGINT -> cleanup\n";
    cleanup();
    _exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " M N K\nExample: " << argv[0] << " 4 3 5\n";
        return 1;
    }
    int M = std::atoi(argv[1]);
    int N = std::atoi(argv[2]);
    int K = std::atoi(argv[3]);

    if (M<=0||N<=0||K<=0 || M>MAX_ROWS||N>MAX_SHELVES_PER_ROW||K>MAX_BOOKS_PER_SHELF) {
        std::cerr << "Bad args\n";
        return 1;
    }

    std::signal(SIGINT, on_sigint);

    // чистим старые артефакты предыдущих запусков
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_PORTFOLIO_NAME);
    sem_unlink(SEM_CATALOG_NAME);

    int fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0666);
    if (fd == -1) die("shm_open");
    if (ftruncate(fd, sizeof(SharedData)) == -1) die("ftruncate");

    g_shm = (SharedData*)mmap(nullptr, sizeof(SharedData), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm == MAP_FAILED) die("mmap");
    close(fd);

    auto* shm = g_shm;
    std::memset(shm, 0, sizeof(SharedData));

    shm->M_rows = M;
    shm->N_shelves = N;
    shm->K_per_shelf = K;
    shm->total_books = M*N*K;

    g_sem_portfolio = sem_open(SEM_PORTFOLIO_NAME, O_CREAT|O_EXCL, 0666, 1);
    if (g_sem_portfolio == SEM_FAILED) die("sem_open portfolio");

    g_sem_catalog = sem_open(SEM_CATALOG_NAME, O_CREAT|O_EXCL, 0666, 1);
    if (g_sem_catalog == SEM_FAILED) die("sem_open catalog");

    // случайная раскладка
    std::vector<int> ids(shm->total_books);
    for (int i=0;i<shm->total_books;++i) ids[i]=i+1;
    std::mt19937 rng((unsigned)time(nullptr));
    std::shuffle(ids.begin(), ids.end(), rng);

    int idx=0;
    for (int r=0;r<M;++r)
        for (int s=0;s<N;++s)
            for (int p=0;p<K;++p)
                shm->book_at_pos[r][s][p] = ids[idx++];

    shm->initialized = 1;

    std::cout << "[INIT] Ready: M="<<M<<" N="<<N<<" K="<<K<<" total="<<shm->total_books<<"\n";
    std::cout << "[INIT] Run workers (in other terminals): ./worker <id>\n";
    std::cout << "[INIT] Optional observers: ./observer (can be many)\n";

    // ждём окончания: rows_done == M
    while (true) {
        sem_wait(g_sem_catalog);
        int done = shm->rows_done;
        int cat  = shm->catalog_count;
        sem_post(g_sem_catalog);

        std::cout << "[INIT] progress: rows_done=" << done << "/" << M
                  << " catalog=" << cat << "/" << shm->total_books << "\n";

        if (done >= M && cat == shm->total_books) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::cout << "\n[INIT] FINAL CATALOG (" << shm->catalog_count << " entries)\n";
    for (int i=0;i<shm->catalog_count;++i) print_entry(shm->catalog[i]);

    cleanup();
    std::cout << "[INIT] Resources removed.\n";
    return 0;
}
