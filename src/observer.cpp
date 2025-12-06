#define _XOPEN_SOURCE 700

#include "lib_common.hpp"

#include <iostream>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>

#include <mqueue.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#include <errno.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static void die(const char* msg) { perror(msg); std::exit(1); }

static void unregister_observer(SharedData* shm, sem_t* sem_catalog, const char* qname) {
    sem_wait(sem_catalog);
    for (int i=0;i<shm->observers_count;++i) {
        if (std::strncmp(shm->observer_queues[i], qname, OBS_NAME_LEN) == 0) {
            int last = shm->observers_count - 1;
            if (i != last) std::memcpy(shm->observer_queues[i], shm->observer_queues[last], OBS_NAME_LEN);
            std::memset(shm->observer_queues[last], 0, OBS_NAME_LEN);
            shm->observers_count--;
            break;
        }
    }
    sem_post(sem_catalog);
}

int main() {
    std::signal(SIGINT, on_sigint);

    // уникальное имя очереди наблюдателя
    char qname[OBS_NAME_LEN]{};
    std::snprintf(qname, sizeof(qname), "/inv_obs_%d", (int)getpid());

    // создаём свою очередь
    mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 256;
    attr.mq_curmsgs = 0;


    mq_unlink(qname); // на всякий случай
    mqd_t mq = mq_open(qname, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq == (mqd_t)-1) die("mq_open(observer)");

    std::cout << "[OBSERVER] queue=" << qname << "\n";

    // подключаемся к shm и sem (в любом порядке запуска)
    int fd = -1;
    for (;;) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd != -1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto* shm = (SharedData*)mmap(nullptr, sizeof(SharedData), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("mmap");
    close(fd);

    sem_t* sem_catalog = SEM_FAILED;
    for (;;) {
        sem_catalog = sem_open(SEM_CATALOG_NAME, 0);
        if (sem_catalog != SEM_FAILED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ждём init
    while (!shm->initialized) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // регистрируемся в shared memory
    sem_wait(sem_catalog);
    if (shm->observers_count < MAX_OBSERVERS) {
        std::strncpy(shm->observer_queues[shm->observers_count], qname, OBS_NAME_LEN-1);
        shm->observers_count++;
    } else {
        std::cerr << "[OBSERVER] too many observers\n";
    }
    sem_post(sem_catalog);

    std::cout << "[OBSERVER] registered. Ctrl+C to exit.\n";

    char buf[256];
    while (!g_stop) {
        ssize_t n = mq_receive(mq, buf, sizeof(buf), nullptr);
        if (n >= 0) {
            buf[n] = '\0';
            std::cout << "[OBSERVER] " << buf << "\n";
        } else {
            if (errno == EINTR) continue;
            // блокирующий receive обычно не выдаёт EAGAIN, но пусть будет безопасно:
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::cout << "[OBSERVER] exiting...\n";
    unregister_observer(shm, sem_catalog, qname);

    sem_close(sem_catalog);
    munmap(shm, sizeof(SharedData));
    mq_close(mq);
    mq_unlink(qname);
    return 0;
}
