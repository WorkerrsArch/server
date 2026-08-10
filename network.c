// -std=c11 (в отличие от -std=gnu11) отключает POSIX-расширения glibc —
// без этого clock_gettime/CLOCK_MONOTONIC/getaddrinfo/struct addrinfo
// не объявлены. Должно стоять до ЛЮБОГО #include в файле.
#define _POSIX_C_SOURCE 200809L

#include "network.h"
#ifndef NETWORK_HEADLESS_BUILD
    #include "raymath.h"
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <netinet/tcp.h>   // TCP_NODELAY
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

// ===================== Протокол поверх TCP =====================
// TCP - поток байт без границ сообщений, поэтому каждое сообщение
// кадрируется заголовком: [uint32 type][uint32 length][payload...].
// Раньше (на UDP) тип сообщения угадывался по размеру датаграммы -
// с TCP так нельзя, отправитель и получатель работают с общим потоком.
typedef enum {
    MSG_HELLO      = 1, // клиент -> сервер, первичное приветствие, payload = PlayerState
    MSG_STATE      = 2, // клиент -> сервер, обновление состояния,   payload = PlayerState
    MSG_WELCOME    = 3, // сервер -> клиент, назначение id,          payload = WelcomeMsg
    MSG_SNAPSHOT   = 4, // сервер -> клиент, снапшот игры,           payload = GameSnapshot
    MSG_HITEVENT   = 5, // клиент -> сервер, выстрел,                payload = HitEvent
    MSG_HITCONFIRM = 6, // сервер -> клиент, результат попадания,    payload = HitConfirm
} MsgType;

#define RECV_STREAM_BUF 2048

bool isServer = false;
static int myId = -1;
static bool hostPlaysToo = true; // false для настоящего dedicated-сервера (server_main.c)

// ---- серверное состояние ----
static SOCKET listenSock = INVALID_SOCKET;
static SOCKET clientSockets[MAX_PLAYERS];
static PlayerState clientStates[MAX_PLAYERS];
static bool clientConnected[MAX_PLAYERS];
static double clientLastSeen[MAX_PLAYERS];
static char clientRecvBuf[MAX_PLAYERS][RECV_STREAM_BUF];
static int clientRecvLen[MAX_PLAYERS];
static int clientCount = 0;

// ---- клиентское состояние (соединение с сервером) ----
static SOCKET sock = INVALID_SOCKET;
static char recvStreamBuf[RECV_STREAM_BUF];
static int recvStreamLen = 0;

// Очередь HitConfirm-пакетов, полученных клиентом (порядок обработки
// в Gameplay_Update не совпадает с порядком чтения кадров из сокета).
static HitConfirm pendingConfirms[MAX_PACKETS_PER_TICK];
static int pendingConfirmCount = 0;

static bool InitSockets(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
#endif
    return true;
}

// Раньше сервер брал время через raylib GetTime(), а вызывался он до
// InitWindow() (см. main.c) - таймер raylib на этот момент ещё не
// инициализирован. Плюс headless-сборке raylib вообще не сдался.
// Поэтому у сети теперь свои независимые монотонные часы.
static double Net_Now(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void SetNonBlocking(SOCKET s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Без этого TCP-стек (алгоритм Нейгла) придерживает мелкие пакеты
// (PlayerState шлётся каждый тик - как раз "мелкий" в терминах TCP),
// ожидая либо накопления данных до MSS, либо ACK на предыдущий сегмент.
// В связке с delayed ACK на другой стороне это стабильно добавляет
// 40-200мс к каждому пакету - именно это ощущается как "высокий пинг"
// при полностью рабочей сети. Ставим на КАЖДЫЙ сокет, который шлёт
// данные: клиентский sock и каждый accept()-нутый серверный сокет.
static void SetTcpNoDelay(SOCKET s) {
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
}

// Досылает данные целиком: send() на TCP может отправить меньше, чем
// попросили, если буфер сокета временно полон.
static bool SendAll(SOCKET s, const void *data, size_t len) {
    const char *p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n > 0) { sent += (size_t)n; continue; }
#ifdef _WIN32
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) continue;
#endif
        return false; // реальный разрыв соединения / ошибка сокета
    }
    return true;
}

static bool SendFramed(SOCKET s, uint32_t type, const void *payload, uint32_t len) {
    if (s == INVALID_SOCKET) return false;
    uint32_t header[2] = { type, len };
    if (!SendAll(s, header, sizeof(header))) return false;
    if (len > 0 && !SendAll(s, payload, len)) return false;
    return true;
}

// Читает всё, что накопилось в сокете, в потоковый буфер (неблокирующе).
// false = соединение разорвано (клиент отключился / ошибка сокета).
static bool PumpRecv(SOCKET s, char *buf, int *bufLen) {
    while (*bufLen < RECV_STREAM_BUF) {
        int n = recv(s, buf + *bufLen, RECV_STREAM_BUF - *bufLen, 0);
        if (n > 0) { *bufLen += n; continue; }
        if (n == 0) return false; // штатное закрытие соединения
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
        if (errno == EWOULDBLOCK || errno == EAGAIN) break;
#endif
        return false; // ошибка сокета
    }
    return true;
}

// Пытается вытащить один целый кадр из потокового буфера. Если кадр
// готов - копирует payload в outPayload и сдвигает остаток буфера.
static bool TryParseFrame(char *buf, int *bufLen, uint32_t *outType,
                           char *outPayload, uint32_t maxPayload, uint32_t *outLen) {
    if (*bufLen < (int)(2 * sizeof(uint32_t))) return false;
    uint32_t type, len;
    memcpy(&type, buf, 4);
    memcpy(&len, buf + 4, 4);
    uint32_t total = 8 + len;
    if (len > maxPayload) {
        // не наш протокол / повреждённый поток - сбрасываем буфер,
        // чтобы не зависнуть в ожидании кадра, который никогда не влезет
        *bufLen = 0;
        return false;
    }
    if ((uint32_t)*bufLen < total) return false; // кадр ещё не пришёл целиком
    *outType = type;
    *outLen = len;
    if (len > 0) memcpy(outPayload, buf + 8, len);
    memmove(buf, buf + total, (size_t)(*bufLen - (int)total));
    *bufLen -= (int)total;
    return true;
}

static bool StartListenSocket(int port) {
    if (!InitSockets()) return false;
    listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) return false;

    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(listenSock); listenSock = INVALID_SOCKET; return false;
    }
    if (listen(listenSock, 16) < 0) {
        closesocket(listenSock); listenSock = INVALID_SOCKET; return false;
    }
    SetNonBlocking(listenSock);

    for (int i = 0; i < MAX_PLAYERS; i++) { clientSockets[i] = INVALID_SOCKET; clientConnected[i] = false; }
    isServer = true;
    return true;
}

bool Network_InitServer(void) {
    if (!StartListenSocket(SERVER_PORT)) return false;
    hostPlaysToo = true;
    myId = 0;
    clientCount = 1;
    clientStates[0] = (PlayerState){ .health = 100, .alive = true };
    clientConnected[0] = true;
    clientLastSeen[0] = Net_Now();
    printf("TCP listen-server started on port %d\n", SERVER_PORT);
    return true;
}

bool Network_InitDedicatedServer(int port) {
    if (!StartListenSocket(port)) return false;
    hostPlaysToo = false;
    myId = -1;      // у dedicated-сервера нет своего игрока
    clientCount = 0;
    printf("Dedicated TCP server started on port %d\n", port);
    return true;
}

bool Network_InitClient(const char *serverAddr) {
    if (!InitSockets()) return false;

    // Принимаем и "host", и "host:port" - второе нужно для Railway,
    // где публичный порт TCP Proxy назначается динамически и приходит
    // вместе с доменом (RAILWAY_TCP_PROXY_DOMAIN:RAILWAY_TCP_PROXY_PORT).
    char host[128];
    int port = SERVER_PORT;
    const char *colon = strrchr(serverAddr, ':');
    if (colon) {
        size_t hostLen = (size_t)(colon - serverAddr);
        if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, serverAddr, hostLen);
        host[hostLen] = '\0';
        int parsedPort = atoi(colon + 1);
        if (parsedPort > 0) port = parsedPort;
    } else {
        snprintf(host, sizeof(host), "%s", serverAddr);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;

    // getaddrinfo вместо inet_pton - нужно резолвить и IP, и хостнейм
    // (Railway даёт именно домен вида xxx.proxy.rlwy.net, не IP).
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
        closesocket(sock); sock = INVALID_SOCKET; return false;
    }
    // Подключаемся блокирующим вызовом - это разовая операция при старте,
    // асинхронный connect() усложнил бы код без реальной пользы здесь.
    bool connected = (connect(sock, res->ai_addr, (int)res->ai_addrlen) == 0);
    freeaddrinfo(res);
    if (!connected) {
        closesocket(sock); sock = INVALID_SOCKET; return false;
    }
    SetNonBlocking(sock);
    SetTcpNoDelay(sock);
    recvStreamLen = 0;

    isServer = false;
    myId = -1;

    PlayerState hello = { .health = 100, .alive = true };
    SendFramed(sock, MSG_HELLO, &hello, sizeof(hello));

    printf("Client connected to %s:%d (TCP)\n", host, port);
    return true;
}

void Network_SendState(PlayerState state) {
    if (isServer) {
        if (!hostPlaysToo) return; // dedicated-сервер сам не играет
        // health/alive авторитетны только на сервере - иначе жертва каждый
        // кадр присылает своё "неповреждённое" состояние и затирает урон.
        // Исключение - реальный респаун (клиент был мёртв, стал жив).
        bool respawn = state.alive && !clientStates[0].alive;
        float authHealth = clientStates[0].health;
        bool authAlive = clientStates[0].alive;
        clientStates[0] = state;
        if (!respawn) {
            clientStates[0].health = authHealth;
            clientStates[0].alive = authAlive;
        }
    } else {
        if (sock == INVALID_SOCKET) return;
        SendFramed(sock, MSG_STATE, &state, sizeof(state));
    }
}

static void ServerCheckTimeouts(void) {
    double now = Net_Now();
    int base = hostPlaysToo ? 1 : 0;
    for (int i = base; i < clientCount; i++) {
        if (clientConnected[i] && now - clientLastSeen[i] > CLIENT_TIMEOUT) {
            printf("Client %d timed out, disconnecting\n", i);
            if (clientSockets[i] != INVALID_SOCKET) { closesocket(clientSockets[i]); clientSockets[i] = INVALID_SOCKET; }
            clientConnected[i] = false;
            clientStates[i].alive = false;
        }
    }
}

void Network_BroadcastHitConfirm(HitConfirm confirm) {
    if (!isServer) return;
    int base = hostPlaysToo ? 1 : 0;
    for (int i = base; i < clientCount; i++) {
        if (!clientConnected[i]) continue;
        SendFramed(clientSockets[i], MSG_HITCONFIRM, &confirm, sizeof(confirm));
    }
}

// Общая логика попадания: и когда HitEvent пришёл по сети от клиента,
// и когда стреляет сам хост (у которого нет сетевого RTT до самого себя,
// см. Network_HostFire). Считает урон, обновляет clientStates[] и
// рассылает HitConfirm всем клиентам.
static void ResolveHitEvent(HitEvent event) {
    float bestDist = HITSCAN_RANGE;
    int bestTarget = -1;
    for (int i = 0; i < clientCount; i++) {
        if (i == event.shooterId || !clientConnected[i] || !clientStates[i].alive) continue;
        Vector3 targetPos = clientStates[i].position;
        Vector3 oc = Vector3Subtract(event.origin, targetPos);
        float b = Vector3DotProduct(oc, event.direction);
        float c = Vector3DotProduct(oc, oc) - 0.25f;
        float disc = b*b - c;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t < 0) t = -b + sqrtf(disc);
        if (t > 0 && t < bestDist) { bestDist = t; bestTarget = i; }
    }

    HitConfirm confirm;
    confirm.shooterId = event.shooterId;
    if (bestTarget != -1) {
        int damage = (event.weaponType == 0) ? 25 : 35;
        clientStates[bestTarget].health -= damage;
        if (clientStates[bestTarget].health <= 0) {
            clientStates[bestTarget].alive = false;
            clientStates[bestTarget].health = 0;
        }
        confirm.targetId = bestTarget;
        confirm.damage = damage;
    } else {
        confirm.targetId = -1;
        confirm.damage = 0;
    }

    Network_BroadcastHitConfirm(confirm);
}

// Хост "выстрелить сам в себя по сети" не может - Network_SendHitEvent для
// сервера намеренно ничего не делает (некому это отправлять). Поэтому свой
// собственный выстрел хост обязан резолвить локально через эту функцию,
// иначе его попадания молча теряются. Актуально только для listen-server.
void Network_HostFire(HitEvent event) {
    if (!isServer || !hostPlaysToo) return;
    ResolveHitEvent(event);
}

PlayerState Network_GetSelfAuthoritativeState(void) {
    if (!isServer || !hostPlaysToo) return (PlayerState){0};
    return clientStates[0];
}

static void AcceptNewConnections(void) {
    for (int iter = 0; iter < 8; iter++) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        SOCKET c = accept(listenSock, (struct sockaddr*)&from, &fromLen);
        if (c == INVALID_SOCKET) break;
        SetNonBlocking(c);
        SetTcpNoDelay(c);

        int base = hostPlaysToo ? 1 : 0;
        int idx = -1;
        for (int i = base; i < clientCount; i++) {
            if (!clientConnected[i]) { idx = i; break; }
        }
        if (idx == -1 && clientCount < MAX_PLAYERS) idx = clientCount++;
        if (idx == -1) { closesocket(c); continue; } // сервер полон

        clientSockets[idx] = c;
        clientConnected[idx] = true;
        clientStates[idx] = (PlayerState){0};
        clientRecvLen[idx] = 0;
        clientLastSeen[idx] = Net_Now();

        WelcomeMsg w = { .assignedId = idx };
        SendFramed(c, MSG_WELCOME, &w, sizeof(w));
        printf("New client connected (id=%d)\n", idx);
    }
}

bool Network_ReceiveSnapshot(GameSnapshot *snap) {
    if (isServer) {
        if (listenSock == INVALID_SOCKET) return false;
        bool gotAny = false;

        AcceptNewConnections();

        int base = hostPlaysToo ? 1 : 0;
        for (int i = base; i < clientCount; i++) {
            if (!clientConnected[i]) continue;
            SOCKET cs = clientSockets[i];

            if (!PumpRecv(cs, clientRecvBuf[i], &clientRecvLen[i])) {
                printf("Client %d disconnected\n", i);
                closesocket(cs);
                clientSockets[i] = INVALID_SOCKET;
                clientConnected[i] = false;
                clientStates[i].alive = false;
                continue;
            }

            for (int iter = 0; iter < MAX_PACKETS_PER_TICK; iter++) {
                uint32_t type, len;
                char payload[BUFFER_SIZE];
                if (!TryParseFrame(clientRecvBuf[i], &clientRecvLen[i], &type, payload, sizeof(payload), &len)) break;
                clientLastSeen[i] = Net_Now();

                if ((type == MSG_HELLO || type == MSG_STATE) && len == sizeof(PlayerState)) {
                    PlayerState newState;
                    memcpy(&newState, payload, sizeof(newState));
                    // Та же логика: не даём клиенту затирать серверный урон
                    // собственным "неповреждённым" отчётом, кроме респауна.
                    bool respawn = newState.alive && !clientStates[i].alive;
                    float authHealth = clientStates[i].health;
                    bool authAlive = clientStates[i].alive;
                    clientStates[i] = newState;
                    if (!respawn) {
                        clientStates[i].health = authHealth;
                        clientStates[i].alive = authAlive;
                    }
                    gotAny = true;
                } else if (type == MSG_HITEVENT && len == sizeof(HitEvent)) {
                    HitEvent event;
                    memcpy(&event, payload, sizeof(event));
                    // shooterId не доверяем содержимому пакета - берём реальный
                    // слот соединения, с которого он пришёл (раньше, на UDP,
                    // клиент сам подставлял свой id, что легко подделать).
                    event.shooterId = i;
                    ResolveHitEvent(event);
                    gotAny = true;
                }
            }
        }

        ServerCheckTimeouts();
        snap->playerCount = clientCount;
        memcpy(snap->players, clientStates, sizeof(PlayerState) * clientCount);
        return gotAny;

    } else {
        if (sock == INVALID_SOCKET) return false;

        if (!PumpRecv(sock, recvStreamBuf, &recvStreamLen)) {
            return false; // сервер разорвал соединение
        }

        bool got = false;
        for (int iter = 0; iter < MAX_PACKETS_PER_TICK; iter++) {
            uint32_t type, len;
            char payload[BUFFER_SIZE];
            if (!TryParseFrame(recvStreamBuf, &recvStreamLen, &type, payload, sizeof(payload), &len)) break;

            if (type == MSG_WELCOME && len == sizeof(WelcomeMsg)) {
                WelcomeMsg w; memcpy(&w, payload, sizeof(w));
                myId = w.assignedId;
            } else if (type == MSG_SNAPSHOT && len == sizeof(GameSnapshot)) {
                memcpy(snap, payload, sizeof(GameSnapshot));
                got = true;
            } else if (type == MSG_HITCONFIRM && len == sizeof(HitConfirm)) {
                if (pendingConfirmCount < MAX_PACKETS_PER_TICK) {
                    memcpy(&pendingConfirms[pendingConfirmCount++], payload, sizeof(HitConfirm));
                }
            }
        }
        return got;
    }
}

void Network_BroadcastSnapshot(GameSnapshot snap) {
    if (!isServer) return;
    int base = hostPlaysToo ? 1 : 0;
    for (int i = base; i < snap.playerCount; i++) {
        if (!clientConnected[i]) continue;
        SendFramed(clientSockets[i], MSG_SNAPSHOT, &snap, sizeof(snap));
    }
}

void Network_SendHitEvent(HitEvent event) {
    if (sock == INVALID_SOCKET || isServer) return;
    SendFramed(sock, MSG_HITEVENT, &event, sizeof(event));
}

// Не используется: на UDP сервер разбирал HitEvent прямо внутри
// Network_ReceiveSnapshot (см. выше), т.к. там же читался сокет. На TCP -
// та же схема, только через ResolveHitEvent(). Оставлено как есть в API
// ради обратной совместимости (объявлена в network.h).
bool Network_ReceiveHitEvent(HitEvent *event) { (void)event; return false; }

bool Network_ReceiveHitConfirm(HitConfirm *confirm) {
    if (sock == INVALID_SOCKET || isServer) return false;
    if (pendingConfirmCount <= 0) return false;
    *confirm = pendingConfirms[--pendingConfirmCount];
    return true;
}

int Network_GetMyId(void) { return myId; }

void Network_Close(void) {
    if (isServer) {
        int base = hostPlaysToo ? 1 : 0;
        for (int i = base; i < MAX_PLAYERS; i++) {
            if (clientSockets[i] != INVALID_SOCKET) { closesocket(clientSockets[i]); clientSockets[i] = INVALID_SOCKET; }
        }
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
    } else {
        if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    }
    isServer = false;
    hostPlaysToo = true;
    myId = -1;
    clientCount = 0;
    pendingConfirmCount = 0;
    recvStreamLen = 0;
    memset(clientConnected, 0, sizeof(clientConnected));
#ifdef _WIN32
    WSACleanup();
#endif
}
