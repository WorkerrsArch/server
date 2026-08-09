// server_main.c — headless dedicated-сервер.
// Не подключает raylib вообще (см. NETWORK_HEADLESS_BUILD в network.h),
// поэтому не рисует, не проигрывает звук и не требует GPU/дисплея —
// годится для обычного Linux-контейнера (Railway и т.п.).
//
// Собирается отдельно от графического клиента:
//   gcc -O2 -DNETWORK_HEADLESS_BUILD -o server server_main.c network.c -lm

#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <signal.h>
#endif

static volatile int running = 1;

#ifndef _WIN32
static void HandleSignal(int sig) { (void)sig; running = 0; }
#endif

static void SleepMs(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

int main(int argc, char **argv) {
    int port = SERVER_PORT;

    // Railway TCP Proxy сам решает, какой публичный порт выдать наружу,
    // но внутри контейнера сервер должен слушать тот порт, который
    // указан как "Internal Port" при включении TCP Proxy в настройках
    // сервиса. По умолчанию это SERVER_PORT (50000) — либо переопределите
    // через переменную окружения PORT / аргумент --port и укажите то же
    // значение в настройках Railway.
    const char *envPort = getenv("PORT");
    if (envPort && atoi(envPort) > 0) port = atoi(envPort);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    }

    if (!Network_InitDedicatedServer(port)) {
        fprintf(stderr, "Failed to start dedicated server on port %d\n", port);
        return 1;
    }
    printf("Dedicated server listening on port %d\n", port);
    fflush(stdout);

#ifndef _WIN32
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);
#endif

    // 30 Гц серверного тика достаточно: сервер только ретранслирует позиции
    // и резолвит попадания, а не считает физику/рендер.
    const int tickMs = 33;
    while (running) {
        GameSnapshot snap;
        if (Network_ReceiveSnapshot(&snap)) {
            Network_BroadcastSnapshot(snap);
        }
        SleepMs(tickMs);
    }

    printf("Shutting down dedicated server...\n");
    Network_Close();
    return 0;
}
