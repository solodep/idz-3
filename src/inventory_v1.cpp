#define _XOPEN_SOURCE 700

#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <csignal>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <semaphore.h>

constexpr const char* SHM_NAME_V1 = "/inventory_v1_shm";

constexpr int MAX_ROWS_V1            = 16;
constexpr int MAX_SHELVES_PER_ROW_V1 = 16;
constexpr int MAX_BOOKS_PER_SHELF_V1 = 32;
constexpr int MAX_BOOKS_V1           = MAX_ROWS_V1 * MAX_SHELVES_PER_ROW_V1 * MAX_BOOKS_PER_SHELF_V1;

struct BookEntryV1 {
    int title, row, shelf, pos;
};

struct SharedV1 {
    int M=0,N=0,K=0,total=0;

    int book_at_pos[MAX_ROWS_V1][MAX_SHELVES_PER_ROW_V1][MAX_BOOKS_PER_SHELF_V1]{};

    int next_row = 0;
    int catalog_count = 0;
    BookEntryV1 catalog[MAX_BOOKS_V1]{};

    sem_t portfolio_mutex; // pshared=1
    sem_t catalog_mutex;   // pshared=1
};

static SharedV1* g_shm = nullptr;
static pid_t g_parent = 0;

static void die(const char* msg) {
    perror(msg);
    std::exit(1);
}

static int cmp_entry(const BookEntryV1& a, const BookEntryV1& b) {
    if (a.title != b.title) return a.title < b.title ? -1 : 1;
    return 0;
}

static void merge_catalog(SharedV1* shm, const std::vector<BookEntryV1>& local) {
    std::vector<BookEntryV1> merged;
    merged.reserve(shm->catalog_count + (int)local.size());

    int i=0, j=0;
    while (i < shm->catalog_count && j < (int)local.size()) {
        if (cmp_entry(shm->catalog[i], local[j]) <= 0) merged.push_back(shm->catalog[i++]);
        else merged.push_back(local[j++]);
    }
    while (i < shm->catalog_count) merged.push_back(shm->catalog[i++]);
    while (j < (int)local.size()) merged.push_back(local[j++]);

    std::memcpy(shm->catalog, merged.data(), merged.size() * sizeof(BookEntryV1));
    shm->catalog_count = (int)merged.size();
}

static void cleanup_parent() {
    if (!g_shm) return;
    sem_destroy(&g_shm->portfolio_mutex);
    sem_destroy(&g_shm->catalog_mutex);
    munmap(g_shm, sizeof(SharedV1));
    shm_unlink(SHM_NAME_V1);
    g_shm = nullptr;
}

static void on_sigint(int) {
    if (getpid() == g_parent) {
        std::cerr << "\n[Parent] SIGINT -> cleanup\n";
        cleanup_parent();
    }
    _exit(1);
}

static void worker_loop(SharedV1* shm, int wid) {
    while (true) {
        sem_wait(&shm->portfolio_mutex);
        int row = shm->next_row;
        if (row >= shm->M) {
            sem_post(&shm->portfolio_mutex);
            std::cout << "[Worker " << wid << "] no more rows\n";
            break;
        }
        shm->next_row++;
        sem_post(&shm->portfolio_mutex);

        std::cout << "[Worker " << wid << "] processing row " << row << "\n";

        std::vector<BookEntryV1> local;
        local.reserve(shm->N * shm->K);

        for (int s=0; s<shm->N; ++s)
            for (int p=0; p<shm->K; ++p)
                local.push_back({shm->book_at_pos[row][s][p], row, s, p});

        std::sort(local.begin(), local.end(), [](auto& a, auto& b){ return a.title < b.title; });

        sem_wait(&shm->catalog_mutex);
        merge_catalog(shm, local);
        sem_post(&shm->catalog_mutex);

        std::cout << "[Worker " << wid << "] finished row " << row << "\n";
    }
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " M N K workers\n";
        return 1;
    }
    int M = std::atoi(argv[1]);
    int N = std::atoi(argv[2]);
    int K = std::atoi(argv[3]);
    int P = std::atoi(argv[4]);

    if (M<=0||N<=0||K<=0||P<=0 || M>MAX_ROWS_V1||N>MAX_SHELVES_PER_ROW_V1||K>MAX_BOOKS_PER_SHELF_V1)
        return (std::cerr << "Bad args\n", 1);

    g_parent = getpid();
    std::signal(SIGINT, on_sigint);

    int fd = shm_open(SHM_NAME_V1, O_CREAT|O_RDWR, 0666);
    if (fd == -1) die("shm_open");
    if (ftruncate(fd, sizeof(SharedV1)) == -1) die("ftruncate");

    g_shm = (SharedV1*)mmap(nullptr, sizeof(SharedV1), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm == MAP_FAILED) die("mmap");
    close(fd);

    auto* shm = g_shm;
    std::memset(shm, 0, sizeof(SharedV1));

    shm->M=M; shm->N=N; shm->K=K; shm->total = M*N*K;

    if (sem_init(&shm->portfolio_mutex, 1, 1) == -1) die("sem_init portfolio");
    if (sem_init(&shm->catalog_mutex,   1, 1) == -1) die("sem_init catalog");

    std::vector<int> ids(shm->total);
    for (int i=0;i<shm->total;++i) ids[i]=i+1;

    std::mt19937 rng((unsigned)time(nullptr));
    std::shuffle(ids.begin(), ids.end(), rng);

    int idx=0;
    for (int r=0;r<M;++r)
        for (int s=0;s<N;++s)
            for (int p=0;p<K;++p)
                shm->book_at_pos[r][s][p] = ids[idx++];

    std::cout << "[Parent] init done. Forking " << P << " workers...\n";

    for (int i=0;i<P;++i) {
        pid_t pid = fork();
        if (pid < 0) die("fork");
        if (pid == 0) {
            worker_loop(shm, i);
            return 0;
        }
    }

    for (int i=0;i<P;++i) wait(nullptr);

    std::cout << "\n[Parent] Final catalog (" << shm->catalog_count << "):\n";
    for (int i=0;i<shm->catalog_count;++i) {
        auto& e = shm->catalog[i];
        std::printf("book=%4d  row=%2d shelf=%2d pos=%2d\n", e.title, e.row, e.shelf, e.pos);
    }

    cleanup_parent();
    return 0;
}
