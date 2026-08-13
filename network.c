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
    #include <sys/select.h>   // select() для ConnectWithTimeout
    #include <sys/stat.h>     // mkdir
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
    MSG_PING       = 7, // клиент -> сервер, замер RTT,              payload = double (timestamp)
    MSG_PONG       = 8, // сервер -> клиент, эхо на MSG_PING,        payload = double (тот же timestamp)
    MSG_MAP_INFO   = 9, // сервер -> клиент: {uint32 crc, uint32 size}
    MSG_MAP_NEED   = 10,// клиент -> сервер: нужна полная карта
    MSG_MAP_HAVE   = 11,// клиент -> сервер: локальный кэш совпал по CRC
    MSG_MAP_CHUNK  = 12,// сервер -> клиент: {uint32 offset, uint32 total, data...}
} MsgType;

#define RECV_STREAM_BUF 16384
#define MAP_CHUNK_PAYLOAD 4096
#define MAP_MAX_BYTES (24 * 5012 * 5012)

bool isServer = false;
static int myId = -1;
static bool hostPlaysToo = true; // false для настоящего dedicated-сервера (server_main.c)

// ---- карта для раздачи ----
static unsigned char *mapFileData = NULL;
static uint32_t mapFileSize = 0;
static uint32_t mapFileCrc = 0;

// Сетка блоков для серверного raycast (анти-wallhack).
// Размеры совпадают с world.h (WORLD_X/Y/Z). Порядок в файле: x, z, y.
#define SRV_WX 100
#define SRV_WY 99
#define SRV_WZ 100
#define SRV_AIR ((signed char)-1)
static signed char *srvBlocks = NULL;
static int srvMapReady = 0;

static inline int SrvBlockIndex(int x, int y, int z) {
    return (x * SRV_WZ + z) * SRV_WY + y;
}

static inline signed char SrvGetBlock(int x, int y, int z) {
    if (x < 0 || x >= SRV_WX || y < 0 || y >= SRV_WY || z < 0 || z >= SRV_WZ)
        return SRV_AIR;
    return srvBlocks[SrvBlockIndex(x, y, z)];
}

static void ServerFreeBlockGrid(void) {
    if (srvBlocks) { free(srvBlocks); srvBlocks = NULL; }
    srvMapReady = 0;
}

// Разобрать mapFileData → srvBlocks (формат World_Save/World_Load).
static void ServerBuildBlockGrid(void) {
    ServerFreeBlockGrid();
    if (!mapFileData || mapFileSize < 12) return;

    int hx = 0, hy = 0, hz = 0;
    memcpy(&hx, mapFileData + 0, 4);
    memcpy(&hy, mapFileData + 4, 4);
    memcpy(&hz, mapFileData + 8, 4);
    if (hx != SRV_WX || hy != SRV_WY || hz != SRV_WZ) {
        printf("Map dims %dx%dx%d != server %dx%dx%d — raycast off\n",
               hx, hy, hz, SRV_WX, SRV_WY, SRV_WZ);
        return;
    }
    size_t voxels = (size_t)hx * (size_t)hy * (size_t)hz;
    if (mapFileSize < 12 + voxels) {
        printf("Map file too small for block grid\n");
        return;
    }
    srvBlocks = (signed char *)malloc(voxels);
    if (!srvBlocks) return;

    const unsigned char *p = mapFileData + 12;
    for (int x = 0; x < hx; x++)
        for (int z = 0; z < hz; z++)
            for (int y = 0; y < hy; y++) {
                // (unsigned char)255 при записи AIR(-1) → signed char -1
                srvBlocks[SrvBlockIndex(x, y, z)] = (signed char)(*p++);
            }
    srvMapReady = 1;
    printf("Server block grid ready (%zu voxels) — wall occlusion ON\n", voxels);
}

// DDA по вокселям (как World_Raycast). Возвращает t до первого solid-блока,
// либо maxDist+1 если стена не встретилась.
static float ServerBlockRaycast(Vector3 o, Vector3 d, float maxDist) {
    if (!srvMapReady) return maxDist + 1.0f;

    float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (len2 < 1e-12f) return maxDist + 1.0f;
    float inv = 1.0f / sqrtf(len2);
    d.x *= inv; d.y *= inv; d.z *= inv;

    int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z);
    int sx = (d.x > 0.0f) ? 1 : -1;
    int sy = (d.y > 0.0f) ? 1 : -1;
    int sz = (d.z > 0.0f) ? 1 : -1;
    float tdx = (d.x != 0.0f) ? fabsf(1.0f / d.x) : 1e30f;
    float tdy = (d.y != 0.0f) ? fabsf(1.0f / d.y) : 1e30f;
    float tdz = (d.z != 0.0f) ? fabsf(1.0f / d.z) : 1e30f;
    float tmx = (d.x != 0.0f) ? ((d.x > 0.0f ? (x + 1 - o.x) : (o.x - x)) * tdx) : 1e30f;
    float tmy = (d.y != 0.0f) ? ((d.y > 0.0f ? (y + 1 - o.y) : (o.y - y)) * tdy) : 1e30f;
    float tmz = (d.z != 0.0f) ? ((d.z > 0.0f ? (z + 1 - o.z) : (o.z - z)) * tdz) : 1e30f;
    float t = 0.0f;

    // если стартуем внутри solid (редкий глюк) — игнорируем первую клетку
    int steps = 0;
    const int maxSteps = SRV_WX + SRV_WY + SRV_WZ + 8;
    while (t < maxDist && steps++ < maxSteps) {
        if (SrvGetBlock(x, y, z) != SRV_AIR) {
            // первая клетка с t≈0 — стрелок внутри блока, пропускаем
            if (t > 0.001f) return t;
        }
        if (tmx < tmy) {
            if (tmx < tmz) { x += sx; t = tmx; tmx += tdx; }
            else           { z += sz; t = tmz; tmz += tdz; }
        } else {
            if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; }
            else           { z += sz; t = tmz; tmz += tdz; }
        }
    }
    return maxDist + 1.0f;
}

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

// ---- пинг (RTT) клиента до сервера ----
static double lastPingSentAt = 0.0;
static bool pingWaiting = false;
static float currentPingMs = 0.0f;

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

// Обычный блокирующий connect() может зависать на десятки секунд, если
// хост недоступен (пакеты просто теряются, а не приходит быстрый RST,
// как при "порт закрыт, но хост жив"). Раньше именно так подключение к
// оффлайн-серверу зависало саму игру. Здесь - неблокирующий connect()
// с явным таймаутом через select().
static bool ConnectWithTimeout(SOCKET s, struct addrinfo *res, int timeoutMs) {
    SetNonBlocking(s);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (rc == 0) return true; // мгновенный коннект (localhost и т.п.)

#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
#else
    if (errno != EINPROGRESS) return false;
#endif

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int sel = select((int)s + 1, NULL, &wf, NULL, &tv);
    if (sel <= 0) return false; // таймаут либо ошибка select() - считаем оффлайн

    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
    return err == 0;
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

// ---- CRC32 (ISO HDLC) для проверки карты ----
static uint32_t Crc32Update(uint32_t crc, const unsigned char *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1u)));
    }
    return ~crc;
}

static uint32_t Crc32FileBuffer(const unsigned char *data, size_t len) {
    return Crc32Update(0, data, len);
}

static uint32_t Crc32Path(const char *path, uint32_t *outSize) {
    if (outSize) *outSize = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t crc = 0;
    unsigned char buf[8192];
    size_t n;
    uint32_t total = 0;
    // стартуем с crc=0 → Crc32Update инвертирует; для кусков нужен другой путь
    crc = 0xFFFFFFFFu;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += (uint32_t)n;
        for (size_t i = 0; i < n; i++) {
            crc ^= buf[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1u)));
        }
    }
    fclose(f);
    if (outSize) *outSize = total;
    return ~crc;
}

bool Network_LoadMapFile(const char *path) {
    if (mapFileData) { free(mapFileData); mapFileData = NULL; }
    mapFileSize = 0;
    mapFileCrc = 0;
    ServerFreeBlockGrid();
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Map file not found: %s (clients will use default arena)\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > MAP_MAX_BYTES) {
        fclose(f);
        printf("Map file invalid size: %ld\n", sz);
        return false;
    }
    mapFileData = (unsigned char *)malloc((size_t)sz);
    if (!mapFileData) { fclose(f); return false; }
    if (fread(mapFileData, 1, (size_t)sz, f) != (size_t)sz) {
        free(mapFileData); mapFileData = NULL; fclose(f); return false;
    }
    fclose(f);
    mapFileSize = (uint32_t)sz;
    mapFileCrc = Crc32FileBuffer(mapFileData, mapFileSize);
    printf("Map loaded for serving: %s (%u bytes, crc=0x%08X)\n", path, mapFileSize, mapFileCrc);
    ServerBuildBlockGrid();
    return true;
}

const char *Network_MapCachePath(void) { return NETWORK_MAP_CACHE_PATH; }
uint32_t Network_MapCrc(void) { return mapFileCrc; }
uint32_t Network_MapSize(void) { return mapFileSize; }

static void SendMapInfo(SOCKET s) {
    uint32_t info[2] = { mapFileCrc, mapFileSize };
    SendFramed(s, MSG_MAP_INFO, info, sizeof(info));
}

static void SendMapChunks(SOCKET s) {
    if (!mapFileData || mapFileSize == 0) return;
    uint32_t offset = 0;
    while (offset < mapFileSize) {
        uint32_t chunk = mapFileSize - offset;
        if (chunk > MAP_CHUNK_PAYLOAD) chunk = MAP_CHUNK_PAYLOAD;
        unsigned char packet[8 + MAP_CHUNK_PAYLOAD];
        memcpy(packet + 0, &offset, 4);
        memcpy(packet + 4, &mapFileSize, 4);
        memcpy(packet + 8, mapFileData + offset, chunk);
        if (!SendFramed(s, MSG_MAP_CHUNK, packet, 8 + chunk)) {
            printf("Failed to send map chunk at %u\n", offset);
            return;
        }
        offset += chunk;
    }
    printf("Map fully sent (%u bytes)\n", mapFileSize);
}

// Клиент: дождаться WELCOME+MAP_INFO, сверить CRC с кэшем, при необходимости скачать.
static bool ClientSyncMap(void) {
    bool gotWelcome = false;
    bool gotMapInfo = false;
    uint32_t remoteCrc = 0, remoteSize = 0;
    double deadline = Net_Now() + 15.0;

    while (Net_Now() < deadline && !(gotWelcome && gotMapInfo)) {
        if (!PumpRecv(sock, recvStreamBuf, &recvStreamLen)) return false;
        for (;;) {
            uint32_t type, len;
            char payload[RECV_STREAM_BUF];
            if (!TryParseFrame(recvStreamBuf, &recvStreamLen, &type, payload, sizeof(payload), &len)) break;
            if (type == MSG_WELCOME && len == sizeof(WelcomeMsg)) {
                WelcomeMsg w; memcpy(&w, payload, sizeof(w));
                myId = w.assignedId;
                gotWelcome = true;
            } else if (type == MSG_MAP_INFO && len == 8) {
                memcpy(&remoteCrc, payload, 4);
                memcpy(&remoteSize, payload + 4, 4);
                gotMapInfo = true;
            }
        }
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
#endif
    }
    if (!gotMapInfo) {
        printf("Map sync: no MAP_INFO from server (old server?)\n");
        return true; // совместимость: играем без карты
    }
    printf("Map info from server: size=%u crc=0x%08X\n", remoteSize, remoteCrc);

    if (remoteSize == 0) {
        SendFramed(sock, MSG_MAP_HAVE, NULL, 0);
        // нет кастомной карты — клиент останется на дефолтной арене
        return true;
    }

    uint32_t localSize = 0;
    uint32_t localCrc = Crc32Path(NETWORK_MAP_CACHE_PATH, &localSize);
    if (localSize == remoteSize && localCrc == remoteCrc) {
        printf("Map cache OK (crc match), skip download\n");
        SendFramed(sock, MSG_MAP_HAVE, NULL, 0);
        return true;
    }

    printf("Map cache mismatch — downloading %u bytes...\n", remoteSize);
    SendFramed(sock, MSG_MAP_NEED, NULL, 0);

    unsigned char *buf = (unsigned char *)malloc(remoteSize);
    if (!buf) return false;
    uint32_t received = 0;
    deadline = Net_Now() + 60.0;
    while (received < remoteSize && Net_Now() < deadline) {
        if (!PumpRecv(sock, recvStreamBuf, &recvStreamLen)) { free(buf); return false; }
        for (;;) {
            uint32_t type, len;
            char payload[RECV_STREAM_BUF];
            if (!TryParseFrame(recvStreamBuf, &recvStreamLen, &type, payload, sizeof(payload), &len)) break;
            if (type == MSG_MAP_CHUNK && len >= 8) {
                uint32_t offset, total;
                memcpy(&offset, payload, 4);
                memcpy(&total, payload + 4, 4);
                uint32_t dataLen = len - 8;
                if (total != remoteSize || offset + dataLen > remoteSize) continue;
                memcpy(buf + offset, payload + 8, dataLen);
                if (offset + dataLen > received) received = offset + dataLen;
            } else if (type == MSG_WELCOME && len == sizeof(WelcomeMsg)) {
                WelcomeMsg w; memcpy(&w, payload, sizeof(w));
                myId = w.assignedId;
            }
        }
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
#endif
    }
    if (received < remoteSize) {
        printf("Map download incomplete (%u/%u)\n", received, remoteSize);
        free(buf);
        return false;
    }
    uint32_t gotCrc = Crc32FileBuffer(buf, remoteSize);
    if (gotCrc != remoteCrc) {
        printf("Map CRC mismatch after download\n");
        free(buf);
        return false;
    }
#ifdef _WIN32
    _mkdir("data");
#else
    mkdir("data", 0755);
#endif
    FILE *out = fopen(NETWORK_MAP_CACHE_PATH, "wb");
    if (!out) { free(buf); return false; }
    fwrite(buf, 1, remoteSize, out);
    fclose(out);
    free(buf);
    printf("Map saved to %s\n", NETWORK_MAP_CACHE_PATH);
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
    Network_LoadMapFile(NETWORK_MAP_HOST_PATH);
    printf("TCP listen-server started on port %d\n", SERVER_PORT);
    return true;
}

bool Network_InitDedicatedServer(int port) {
    if (!StartListenSocket(port)) return false;
    hostPlaysToo = false;
    myId = -1;      // у dedicated-сервера нет своего игрока
    clientCount = 0;
    Network_LoadMapFile(NETWORK_MAP_DEDICATED_PATH);
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
    // Таймаут 3 секунды - если сервер оффлайн, не зависаем, а быстро
    // возвращаем false (см. ConnectWithTimeout).
    bool connected = ConnectWithTimeout(sock, res, 3000);
    freeaddrinfo(res);
    if (!connected) {
        closesocket(sock); sock = INVALID_SOCKET; return false;
    }
    SetTcpNoDelay(sock);
    recvStreamLen = 0;
    pendingConfirmCount = 0;
    currentPingMs = 0.0f;
    pingWaiting = false;
    lastPingSentAt = 0.0;

    isServer = false;
    myId = -1;

    PlayerState hello = { .health = 100, .alive = true };
    SendFramed(sock, MSG_HELLO, &hello, sizeof(hello));

    printf("Client connected to %s:%d (TCP)\n", host, port);

    // Синхронизация карты: CRC кэша vs сервер, при отличии — скачать.
    if (!ClientSyncMap()) {
        printf("Map sync failed\n");
        closesocket(sock); sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

// Проверка сервера БЕЗ полноценного подключения: открывает TCP-соединение
// с коротким таймаутом и сразу закрывает его. Используется списком серверов,
// чтобы показать "онлайн/оффлайн" и примерный пинг ДО того, как игрок
// нажмёт "подключиться".
bool Network_ProbeServer(const char *addr, int timeoutMs, float *outPingMs) {
    if (outPingMs) *outPingMs = 0.0f;
    if (!InitSockets()) return false;

    char host[128];
    int port = SERVER_PORT;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t hostLen = (size_t)(colon - addr);
        if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, addr, hostLen);
        host[hostLen] = '\0';
        int parsedPort = atoi(colon + 1);
        if (parsedPort > 0) port = parsedPort;
    } else {
        snprintf(host, sizeof(host), "%s", addr);
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }

    double t0 = Net_Now();
    bool online = ConnectWithTimeout(s, res, timeoutMs);
    double t1 = Net_Now();
    freeaddrinfo(res);
    closesocket(s);

    if (online && outPingMs) *outPingMs = (float)((t1 - t0) * 1000.0);
    return online;
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

// true — состав игроков изменился (connect/disconnect/timeout),
// следующий snapshot нужно обязательно разослать, даже если никто
// не прислал MSG_STATE в этом тике. Иначе у остальных клиентов
// остаётся «призрак» на месте вышедшего игрока.
static bool rosterDirty = false;

// Полностью освободить слот: закрыть сокет, обнулить PlayerState
// (alive=false, position={0}), чтобы в snapshot не уезжала старая поза.
static void ClearClientSlot(int i) {
    if (clientSockets[i] != INVALID_SOCKET) {
        closesocket(clientSockets[i]);
        clientSockets[i] = INVALID_SOCKET;
    }
    clientConnected[i] = false;
    clientStates[i] = (PlayerState){0};
    clientRecvLen[i] = 0;
    rosterDirty = true;
}

static void ServerCheckTimeouts(void) {
    double now = Net_Now();
    int base = hostPlaysToo ? 1 : 0;
    for (int i = base; i < clientCount; i++) {
        if (clientConnected[i] && now - clientLastSeen[i] > CLIENT_TIMEOUT) {
            printf("Client %d timed out, disconnecting\n", i);
            ClearClientSlot(i);
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
    // Нормализуем направление — клиент мог прислать любой вектор.
    float dlen = sqrtf(event.direction.x * event.direction.x +
                       event.direction.y * event.direction.y +
                       event.direction.z * event.direction.z);
    if (dlen < 1e-6f) return;
    event.direction.x /= dlen;
    event.direction.y /= dlen;
    event.direction.z /= dlen;

    // Стена по серверной карте. Если карты нет — occlusion выключен.
    float wallDist = ServerBlockRaycast(event.origin, event.direction, HITSCAN_RANGE);

    float bestDist = HITSCAN_RANGE;
    int bestTarget = -1;
    for (int i = 0; i < clientCount; i++) {
        if (i == event.shooterId || !clientConnected[i] || !clientStates[i].alive) continue;
        Vector3 targetPos = clientStates[i].position;
        Vector3 oc = Vector3Subtract(event.origin, targetPos);
        float b = Vector3DotProduct(oc, event.direction);
        float c = Vector3DotProduct(oc, oc) - 0.25f; // радиус ~0.5 (глаза/торс)
        float disc = b * b - c;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t < 0) t = -b + sqrtf(disc);
        // Попадание только если игрок ближе стены (анти-wallhack).
        if (t > 0.0f && t < bestDist && t < wallDist) {
            bestDist = t;
            bestTarget = i;
        }
    }

    HitConfirm confirm;
    confirm.shooterId = event.shooterId;
    if (bestTarget != -1) {
        int damage = (event.weaponType == 0) ? 25 : 35;
        if (event.headshot) damage = (int)(damage * 2.5f);
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
        rosterDirty = true; // новый игрок — сразу рассылаем snapshot

        WelcomeMsg w = { .assignedId = idx };
        SendFramed(c, MSG_WELCOME, &w, sizeof(w));
        SendMapInfo(c); // CRC+size — клиент решит, качать ли карту
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
                ClearClientSlot(i);
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
                } else if (type == MSG_PING && len == sizeof(double)) {
                    // Просто эхо тем же payload'ом - клиент сам посчитает RTT.
                    SendFramed(cs, MSG_PONG, payload, len);
                } else if (type == MSG_MAP_NEED) {
                    printf("Client %d requested map download\n", i);
                    SendMapChunks(cs);
                } else if (type == MSG_MAP_HAVE) {
                    printf("Client %d has matching map cache\n", i);
                }
            }
        }

        ServerCheckTimeouts();
        snap->playerCount = clientCount;
        memcpy(snap->players, clientStates, sizeof(PlayerState) * clientCount);
        // Обязательно вернуть true при смене состава, иначе disconnect
        // не дойдёт до клиентов и моделька «зависнет» на месте выхода.
        bool dirty = gotAny || rosterDirty;
        rosterDirty = false;
        return dirty;

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
            } else if (type == MSG_PONG && len == sizeof(double)) {
                double sentAt;
                memcpy(&sentAt, payload, sizeof(sentAt));
                currentPingMs = (float)((Net_Now() - sentAt) * 1000.0);
                pingWaiting = false;
            }
        }

        // Раз в секунду меряем RTT до сервера пинг-понгом поверх уже
        // открытого TCP-соединения (сам TCP эту информацию не даёт).
        double now = Net_Now();
        if (!pingWaiting && now - lastPingSentAt > 1.0) {
            double sentAt = now;
            if (SendFramed(sock, MSG_PING, &sentAt, sizeof(sentAt))) {
                lastPingSentAt = now;
                pingWaiting = true;
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

float Network_GetPing(void) {
    if (isServer) return 0.0f; // хост меряет пинг только относительно себя же - нет смысла
    return currentPingMs;
}

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
    currentPingMs = 0.0f;
    pingWaiting = false;
    lastPingSentAt = 0.0;
    memset(clientConnected, 0, sizeof(clientConnected));
    if (mapFileData) { free(mapFileData); mapFileData = NULL; }
    mapFileSize = 0;
    mapFileCrc = 0;
    ServerFreeBlockGrid();
#ifdef _WIN32
    WSACleanup();
#endif
}
