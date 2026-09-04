// server_main.c — headless dedicated-сервер.
// Не подключает raylib (NETWORK_HEADLESS_BUILD).
//
//   gcc -O2 -DNETWORK_HEADLESS_BUILD -o server server_main.c network.c -lm
//
// Тик 60 Гц с компенсацией времени работы: меньше квантования позиции
// (~16 мс вместо 33 мс) → ниже «эффективный пинг» на клиенте.

#define _POSIX_C_SOURCE 200809L

#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <time.h>
    #include <signal.h>
#endif

static volatile int running = 1;

#ifndef _WIN32
static void HandleSignal(int sig) { (void)sig; running = 0; }
#endif

static double NowSec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq, start;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        init = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void SleepMs(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Короткий sleep с субмиллисекундной точностью (остаток тика). */
static void SleepRemaining(double deadline) {
    for (;;) {
        double now = NowSec();
        double left = deadline - now;
        if (left <= 0.0) return;
        if (left > 0.002) {
            /* sleep почти весь остаток, оставив ~0.5мс на spin */
            int ms = (int)((left - 0.0005) * 1000.0);
            if (ms > 0) SleepMs(ms);
        } else {
#ifdef _WIN32
            Sleep(0);
#else
            struct timespec ts = {0, 50 * 1000}; /* 50 мкс */
            nanosleep(&ts, NULL);
#endif
        }
    }
}

int main(int argc, char **argv) {
    int port = SERVER_PORT;
    int tickHz = 60; /* default 60; --hz 30|60|120 */

    const char *envPort = getenv("PORT");
    if (envPort && atoi(envPort) > 0) port = atoi(envPort);
    const char *envHz = getenv("TICK_HZ");
    if (envHz && atoi(envHz) > 0) tickHz = atoi(envHz);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) tickHz = atoi(argv[++i]);
    }
    if (tickHz < 20) tickHz = 20;
    if (tickHz > 120) tickHz = 120;
    const double tickDt = 1.0 / (double)tickHz;

    if (!Network_InitDedicatedServer(port)) {
        fprintf(stderr, "Failed to start dedicated server on port %d\n", port);
        return 1;
    }
    printf("Dedicated server on port %d, tick %d Hz (dt=%.1f ms)\n",
           port, tickHz, tickDt * 1000.0);
    fflush(stdout);

#ifndef _WIN32
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);
#endif

    double nextTick = NowSec();
    unsigned long ticks = 0;
    double lastStat = nextTick;

    while (running) {
        nextTick += tickDt;

        /* 1) принять STATE/HIT/PING, обновить матч */
        GameSnapshot snap;
        Network_ReceiveSnapshot(&snap);

        /* 2) разослать snapshot всем румам сразу после обработки входа —
         *    минимальная задержка STATE→SNAPSHOT на стороне сервера */
        Network_BroadcastSnapshot(snap);

        ticks++;
        double now = NowSec();
        if (now - lastStat >= 10.0) {
            double rate = (double)ticks / (now - lastStat);
            printf("tick rate: %.1f Hz (target %d)\n", rate, tickHz);
            fflush(stdout);
            ticks = 0;
            lastStat = now;
        }

        /* 3) дождаться следующего тика (не Sleep(фиксированные 16мс) —
         *    иначе работа тика + sleep = < target Hz) */
        SleepRemaining(nextTick);

        /* если сильно отстали (долгий GC/IO) — не догоняем пачкой */
        now = NowSec();
        if (now > nextTick + tickDt * 3.0)
            nextTick = now;
    }

    printf("Shutting down dedicated server...\n");
    Network_Close();
    return 0;
}
