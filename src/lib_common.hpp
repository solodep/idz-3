#pragma once

#include <cstdio>

constexpr int MAX_ROWS            = 16;
constexpr int MAX_SHELVES_PER_ROW = 16;
constexpr int MAX_BOOKS_PER_SHELF = 32;
constexpr int MAX_BOOKS           = MAX_ROWS * MAX_SHELVES_PER_ROW * MAX_BOOKS_PER_SHELF;

constexpr int MAX_OBSERVERS       = 16;
constexpr int OBS_NAME_LEN        = 64;

constexpr const char* SHM_NAME           = "/inventory_shm";
constexpr const char* SEM_PORTFOLIO_NAME = "/inv_sem_portfolio";
constexpr const char* SEM_CATALOG_NAME   = "/inv_sem_catalog";

struct BookEntry {
    int title; // ID/название
    int row;
    int shelf;
    int pos;
};

struct SharedData {
    int M_rows = 0;
    int N_shelves = 0;
    int K_per_shelf = 0;
    int total_books = 0;

    int book_at_pos[MAX_ROWS][MAX_SHELVES_PER_ROW][MAX_BOOKS_PER_SHELF]{};

    // портфель задач
    int next_row = 0;
    int rows_done = 0;

    // каталог (всегда отсортирован)
    int catalog_count = 0;
    BookEntry catalog[MAX_BOOKS]{};

    // наблюдатели (каждый имеет свою очередь сообщений)
    int observers_count = 0;
    char observer_queues[MAX_OBSERVERS][OBS_NAME_LEN]{};

    int initialized = 0;
};

inline void print_entry(const BookEntry& e) {
    std::printf("book=%4d  row=%2d shelf=%2d pos=%2d\n", e.title, e.row, e.shelf, e.pos);
}
