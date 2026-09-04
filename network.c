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
    MSG_BOTSCORE   = 13,// клиент -> сервер, очки от локального бота,   payload = BotScoreEvent
    MSG_STATUS_REQ = 14,// probe/клиент -> сервер: запрос статуса
    MSG_STATUS     = 15,// сервер -> клиент: ServerStatusMsg
    MSG_PROFILE_LOGIN = 16, // клиент -> сервер: ProfileLoginMsg
    MSG_PROFILE_INFO  = 17, // сервер -> клиент: ProfileInfoMsg
    MSG_LB_REQ     = 18, // клиент -> сервер: запрос топ-100
    MSG_LB_DATA    = 19, // сервер -> клиент: LeaderboardMsg
    MSG_SPAWN_CFG  = 20, // сервер -> клиент: текст spawn_points.cfg (UTF-8, без нуля)
} MsgType;

#define RECV_STREAM_BUF 16384
#define MAP_CHUNK_PAYLOAD 4096
#define MAP_MAX_BYTES (32 * 1024 * 1024)  // до 32 МБ (custom.dat ~17 МБ ок)

bool isServer = false;
static int myId = -1;
static bool hostPlaysToo = true; // false для настоящего dedicated-сервера (server_main.c)

// ---- матч (скрытые румы на dedicated: до MAX_ROOMS параллельно) ----
typedef struct {
    int phase;
    int scoreShield, scoreVolya;
    float timeLeft;
    double lastTick;
} RoomMatch;
static RoomMatch rooms[MAX_ROOMS];
static int clientRoom[MAX_SERVER_SLOTS];       /* -1 = пусто */
static int clientLocalId[MAX_SERVER_SLOTS];    /* 0..MAX_PLAYERS-1 в руме */
static int myRoomId = 0;                      /* клиент: свой рум (скрыт от UI) */
/* объявлены ниже полностью; здесь — чтобы CountRoomPlayers/Alloc видели */
static bool clientConnected[MAX_SERVER_SLOTS];
static PlayerState clientStates[MAX_SERVER_SLOTS]; /* early: for CountFactionInRoom */
static int lastHitRoom = 0;

static int   clientMatchPhase = MATCH_WAITING;
static int   clientScoreShield = 0, clientScoreVolya = 0;
static float clientTimeLeft = 0.0f;

static void MatchTick(void);
/* Сколько румов реально «открыто» (0..MAX_ROOMS-1).
 * Румы 0..roomsOpen-1 принимают игроков; остальные ждут.
 * При заполнении всех открытых — roomsOpen++ (макс MAX_ROOMS=6).
 * Пустые хвостовые румы схлопываются, чтобы не держать лишнее. */
static int roomsOpen = 1;

static void Rooms_Init(void) {
    roomsOpen = 1;
    for (int r = 0; r < MAX_ROOMS; r++) {
        rooms[r].phase = MATCH_WAITING;
        rooms[r].scoreShield = rooms[r].scoreVolya = 0;
        rooms[r].timeLeft = 0.f;
        rooms[r].lastTick = 0.0;
    }
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        clientRoom[i] = -1;
        clientLocalId[i] = -1;
    }
    printf("Rooms_Init: open=%d max=%d slots=%d\n", roomsOpen, MAX_ROOMS, MAX_SERVER_SLOTS);
}
static int CountRoomPlayers(int r) {
    int n = 0;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++)
        if (clientConnected[i] && clientRoom[i] == r) n++;
    return n;
}
static int CountFactionInRoom(int room, int faction) {
    int n = 0;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (!clientConnected[i] || clientRoom[i] != room) continue;
        if (clientStates[i].faction == faction) n++;
    }
    return n;
}
/* Автобаланс: вернуть фракцию с меньшим числом игроков (0=Щит, 1=Воля). */
static int SuggestFactionForRoom(int room) {
    int s = CountFactionInRoom(room, 0);
    int v = CountFactionInRoom(room, 1);
    return (s <= v) ? 0 : 1;
}
/* Если выбор клиента разбалансирует больше чем на 1 — принудительно.
 * Вызывать при приёме STATE от клиента в руме. */
static int BalanceFaction(int room, int wanted) {
    if (wanted != 0 && wanted != 1) wanted = SuggestFactionForRoom(room);
    int s = CountFactionInRoom(room, 0);
    int v = CountFactionInRoom(room, 1);
    /* не считаем самого клиента в wanted, если он уже в этой фракции —
     * вызывающий передаёт wanted до записи в clientStates. */
    if (wanted == 0) {
        if (s > v + 1) return 1; /* Щит переполнен */
    } else {
        if (v > s + 1) return 0;
    }
    return wanted;
}
static void Rooms_TrimEmpty(void) {
    /* Схлопнуть хвостовые пустые румы, оставив минимум 1 открытый. */
    while (roomsOpen > 1) {
        int last = roomsOpen - 1;
        if (CountRoomPlayers(last) > 0) break;
        rooms[last].phase = MATCH_WAITING;
        rooms[last].scoreShield = rooms[last].scoreVolya = 0;
        rooms[last].timeLeft = 0.f;
        roomsOpen--;
        printf("Rooms: closed empty room %d (open now %d)\n", last, roomsOpen);
    }
}
/* Распределение:
 * 1) Непустой WAITING/COUNTDOWN с местом — самый заполненный (малый онлайн стягиваем).
 * 2) PLAYING с местом, если в руме ещё не «поздно» (есть < MAX и phase playing).
 * 3) Новый пустой среди уже open.
 * 4) Если все open заполнены — открыть следующий (до MAX_ROOMS).
 * 5) Иначе full. */
static int PickRoomForJoin(void) {
    Rooms_TrimEmpty();

    int bestWait = -1, bestWaitN = -1;
    for (int r = 0; r < roomsOpen; r++) {
        int n = CountRoomPlayers(r);
        if (n <= 0 || n >= MAX_PLAYERS) continue;
        if (rooms[r].phase == MATCH_WAITING || rooms[r].phase == MATCH_COUNTDOWN) {
            if (n > bestWaitN) { bestWaitN = n; bestWait = r; }
        }
    }
    if (bestWait >= 0) return bestWait;

    /* PLAYING с дыркой — только если рум не почти полный (иначе лучше новый) */
    int bestPlay = -1, bestPlayN = 999;
    for (int r = 0; r < roomsOpen; r++) {
        int n = CountRoomPlayers(r);
        if (n <= 0 || n >= MAX_PLAYERS) continue;
        if (rooms[r].phase == MATCH_PLAYING) {
            if (n < bestPlayN) { bestPlayN = n; bestPlay = r; }
        }
    }
    if (bestPlay >= 0 && bestPlayN <= MAX_PLAYERS - 2) return bestPlay;

    /* Пустой среди открытых */
    for (int r = 0; r < roomsOpen; r++) {
        if (CountRoomPlayers(r) == 0) {
            rooms[r].phase = MATCH_WAITING;
            rooms[r].scoreShield = rooms[r].scoreVolya = 0;
            rooms[r].timeLeft = 0.f;
            return r;
        }
    }

    /* PLAYING с любым свободным местом */
    if (bestPlay >= 0) return bestPlay;

    /* Все открытые заполнены → открыть новый */
    if (roomsOpen < MAX_ROOMS) {
        int r = roomsOpen;
        roomsOpen++;
        rooms[r].phase = MATCH_WAITING;
        rooms[r].scoreShield = rooms[r].scoreVolya = 0;
        rooms[r].timeLeft = 0.f;
        printf("Rooms: opened room %d (open now %d / %d)\n", r, roomsOpen, MAX_ROOMS);
        return r;
    }
    return -1;
}
static int AllocSlotInRoom(int room) {
    if (room < 0 || room >= MAX_ROOMS) return -1;
    int base = room * MAX_PLAYERS;
    for (int local = 0; local < MAX_PLAYERS; local++) {
        int g = base + local;
        if (g >= MAX_SERVER_SLOTS) break;
        if (!clientConnected[g]) return g;
    }
    return -1;
}
int Network_GetMatchPhase(void) {
    if (!isServer) return clientMatchPhase;
    return rooms[myRoomId >= 0 && myRoomId < MAX_ROOMS ? myRoomId : 0].phase;
}
int Network_GetScoreShield(void) {
    if (!isServer) return clientScoreShield;
    return rooms[myRoomId >= 0 && myRoomId < MAX_ROOMS ? myRoomId : 0].scoreShield;
}
int Network_GetScoreVolya(void) {
    if (!isServer) return clientScoreVolya;
    return rooms[myRoomId >= 0 && myRoomId < MAX_ROOMS ? myRoomId : 0].scoreVolya;
}

/* ---- Серверные профили (жетоны + киллы) ---- */
/* Railway Volume: mount /data. RAILWAY_VOLUME_MOUNT_PATH или /data, иначе cwd. */
#define PROFILE_MAX_STORE 512
static const char *Profiles_Path(void) {
    static char path[256];
    static int inited = 0;
    if (!inited) {
        const char *vol = getenv("RAILWAY_VOLUME_MOUNT_PATH");
        if (vol && vol[0]) {
            snprintf(path, sizeof(path), "%s/profiles.dat", vol);
        } else {
#ifndef _WIN32
            if (access("/data", W_OK) == 0)
                snprintf(path, sizeof(path), "/data/profiles.dat");
            else
#endif
                snprintf(path, sizeof(path), "profiles.dat");
        }
        inited = 1;
        printf("Profiles path: %s\n", path);
    }
    return path;
}
typedef struct {
    char name[PROFILE_NAME_MAX];
    int kills;
    int tokens;
    char ip[46]; /* IPv4/IPv6 текстом, пусто = неизвестно (старые записи) */
} StoredProfile;
static StoredProfile g_profiles[PROFILE_MAX_STORE];
static int g_profileCount = 0;
static bool g_profilesLoaded = false;
static char clientNames[MAX_SERVER_SLOTS][PROFILE_NAME_MAX];
static int  clientProfileIdx[MAX_SERVER_SLOTS]; /* индекс в g_profiles, -1 = нет */
static char clientIP[MAX_SERVER_SLOTS][46];      /* IP подключившегося сокета */

static ProfileInfoMsg pendingProfileInfo;
static bool hasPendingProfileInfo = false;
static LeaderboardMsg cachedLeaderboard;
static bool hasCachedLeaderboard = false;

static int Profile_Find(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_profileCount; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) return i;
    }
    return -1;
}
static void Profile_LoadAll(void) {
    if (g_profilesLoaded) return;
    g_profilesLoaded = true;
    g_profileCount = 0;
    FILE *f = fopen(Profiles_Path(), "r");
    if (!f) return;
    char line[200];
    while (fgets(line, sizeof(line), f) && g_profileCount < PROFILE_MAX_STORE) {
        char name[PROFILE_NAME_MAX];
        int kills = 0, tokens = 0;
        char ip[46]; ip[0] = '\0';
        /* name|kills|tokens|ip */
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = '\0';
        strncpy(name, line, PROFILE_NAME_MAX - 1);
        name[PROFILE_NAME_MAX - 1] = '\0';
        /* trim trailing spaces/CR */
        for (int i = (int)strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\r' || name[i] == '\n'); i--)
            name[i] = '\0';
        if (!name[0]) continue;
        char *p2 = strchr(p1 + 1, '|');
        kills = atoi(p1 + 1);
        tokens = p2 ? atoi(p2 + 1) : 0;
        if (p2) {
            char *p3 = strchr(p2 + 1, '|');
            const char *ipStart = p2 + 1;
            size_t ipLen = p3 ? (size_t)(p3 - ipStart) : strlen(ipStart);
            if (ipLen >= sizeof(ip)) ipLen = sizeof(ip) - 1;
            memcpy(ip, ipStart, ipLen);
            ip[ipLen] = '\0';
            for (int i = (int)strlen(ip) - 1; i >= 0 && (ip[i] == ' ' || ip[i] == '\r' || ip[i] == '\n'); i--)
                ip[i] = '\0';
        }
        if (kills < 0) kills = 0;
        if (tokens < 0) tokens = 0;
        strncpy(g_profiles[g_profileCount].name, name, PROFILE_NAME_MAX - 1);
        g_profiles[g_profileCount].kills = kills;
        g_profiles[g_profileCount].tokens = tokens;
        strncpy(g_profiles[g_profileCount].ip, ip, sizeof(g_profiles[g_profileCount].ip) - 1);
        g_profileCount++;
    }
    fclose(f);
}
static void Profile_SaveAll(void) {
    FILE *f = fopen(Profiles_Path(), "w");
    if (!f) return;
    for (int i = 0; i < g_profileCount; i++)
        fprintf(f, "%s|%d|%d|%s\n", g_profiles[i].name, g_profiles[i].kills,
                g_profiles[i].tokens, g_profiles[i].ip);
    fclose(f);
}
/* Сколько уже зарегистрированных аккаунтов с этого IP (не считая профиль excludeIdx) */
static int Profile_CountByIp(const char *ip, int excludeIdx) {
    if (!ip || !ip[0]) return 0;
    int n = 0;
    for (int i = 0; i < g_profileCount; i++) {
        if (i == excludeIdx) continue;
        if (strcmp(g_profiles[i].ip, ip) == 0) n++;
    }
    return n;
}
/* outLimited: если по IP уже MAX_ACCOUNTS_PER_IP аккаунтов и это НОВЫЙ ник — 
 * профиль не создаётся, возвращается -1 и *outLimited = true. */
static int Profile_GetOrCreate(const char *name, const char *ip, bool *outLimited) {
    Profile_LoadAll();
    if (outLimited) *outLimited = false;
    int idx = Profile_Find(name);
    if (idx >= 0) return idx;
    if (g_profileCount >= PROFILE_MAX_STORE) return -1;
    if (ip && ip[0] && Profile_CountByIp(ip, -1) >= MAX_ACCOUNTS_PER_IP) {
        if (outLimited) *outLimited = true;
        return -1;
    }
    idx = g_profileCount++;
    memset(&g_profiles[idx], 0, sizeof(g_profiles[idx]));
    strncpy(g_profiles[idx].name, name, PROFILE_NAME_MAX - 1);
    g_profiles[idx].name[PROFILE_NAME_MAX - 1] = '\0';
    if (ip) strncpy(g_profiles[idx].ip, ip, sizeof(g_profiles[idx].ip) - 1);
    Profile_SaveAll();
    return idx;
}
static int Profile_RankOf(int idx) {
    if (idx < 0 || idx >= g_profileCount) return 0;
    int rank = 1;
    int k = g_profiles[idx].kills;
    for (int i = 0; i < g_profileCount; i++)
        if (g_profiles[i].kills > k) rank++;
    return rank;
}
static void Profile_BuildLeaderboard(LeaderboardMsg *lb) {
    Profile_LoadAll();
    memset(lb, 0, sizeof(*lb));
    /* простой selection: топ по kills */
    int used[PROFILE_MAX_STORE];
    memset(used, 0, sizeof(used));
    int n = g_profileCount < LEADERBOARD_TOP ? g_profileCount : LEADERBOARD_TOP;
    for (int place = 0; place < n; place++) {
        int best = -1;
        for (int i = 0; i < g_profileCount; i++) {
            if (used[i]) continue;
            if (best < 0 || g_profiles[i].kills > g_profiles[best].kills ||
                (g_profiles[i].kills == g_profiles[best].kills &&
                 g_profiles[i].tokens > g_profiles[best].tokens))
                best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        strncpy(lb->entries[place].name, g_profiles[best].name, PROFILE_NAME_MAX - 1);
        lb->entries[place].kills = g_profiles[best].kills;
        lb->entries[place].tokens = g_profiles[best].tokens;
        lb->count++;
    }
}
static void Profile_AddKill(int clientSlot) {
    if (clientSlot < 0 || clientSlot >= MAX_PLAYERS) return;
    int pidx = clientProfileIdx[clientSlot];
    if (pidx < 0 || pidx >= g_profileCount) return;
    g_profiles[pidx].kills++;
    /* жетон за килл */
    g_profiles[pidx].tokens += 2;
    Profile_SaveAll();
}
static void Profile_AddTokens(int clientSlot, int amount) {
    if (clientSlot < 0 || clientSlot >= MAX_PLAYERS || amount == 0) return;
    int pidx = clientProfileIdx[clientSlot];
    if (pidx < 0 || pidx >= g_profileCount) return;
    g_profiles[pidx].tokens += amount;
    if (g_profiles[pidx].tokens < 0) g_profiles[pidx].tokens = 0;
    Profile_SaveAll();
}
float Network_GetMatchTimeLeft(void) {
    if (!isServer) return clientTimeLeft;
    int r = (myRoomId >= 0 && myRoomId < MAX_ROOMS) ? myRoomId : 0;
    return rooms[r].timeLeft;
}

// ---- карта для раздачи ----
static unsigned char *mapFileData = NULL;
static uint32_t mapFileSize = 0;
static uint32_t mapFileCrc = 0;

/* spawn_points.cfg с dedicated/listen-сервера — приоритет над клиентским файлом */
#define SPAWN_CFG_MAX (64 * 1024)
static char *spawnCfgData = NULL;
static uint32_t spawnCfgSize = 0;
#define NETWORK_SPAWN_SERVER_PATH "server_spawn_points.cfg"
#define NETWORK_SPAWN_LOCAL_PATH  "spawn_points.cfg"

// Сетка блоков для серверного raycast (анти-wallhack).
// Макс. размеры = WORLD_* в world.h. Фактический размер — из заголовка карты.
// Любая карта dims <= MAX работает без правки кода.
#define SRV_MAX_X 350
#define SRV_MAX_Y 150
#define SRV_MAX_Z 350
#define SRV_AIR ((signed char)-1)
static signed char *srvBlocks = NULL;
static int srvMapReady = 0;
static int srvWX = 0, srvWY = 0, srvWZ = 0;

static inline int SrvBlockIndex(int x, int y, int z) {
    return (x * srvWZ + z) * srvWY + y;
}

static inline signed char SrvGetBlock(int x, int y, int z) {
    if (!srvMapReady || x < 0 || x >= srvWX || y < 0 || y >= srvWY || z < 0 || z >= srvWZ)
        return SRV_AIR;
    return srvBlocks[SrvBlockIndex(x, y, z)];
}

static void ServerFreeBlockGrid(void) {
    if (srvBlocks) { free(srvBlocks); srvBlocks = NULL; }
    srvMapReady = 0;
    srvWX = srvWY = srvWZ = 0;
}

// Разобрать mapFileData → srvBlocks (формат World_Save/World_Load).
static void ServerBuildBlockGrid(void) {
    ServerFreeBlockGrid();
    if (!mapFileData || mapFileSize < 12) return;

    int hx = 0, hy = 0, hz = 0;
    memcpy(&hx, mapFileData + 0, 4);
    memcpy(&hy, mapFileData + 4, 4);
    memcpy(&hz, mapFileData + 8, 4);
    if (hx <= 0 || hy <= 0 || hz <= 0 ||
        hx > SRV_MAX_X || hy > SRV_MAX_Y || hz > SRV_MAX_Z) {
        printf("Map dims %dx%dx%d out of range (max %dx%dx%d) — raycast off\n",
               hx, hy, hz, SRV_MAX_X, SRV_MAX_Y, SRV_MAX_Z);
        return;
    }
    size_t voxels = (size_t)hx * (size_t)hy * (size_t)hz;
    if (mapFileSize < 12 + voxels) {
        printf("Map file too small for block grid\n");
        return;
    }
    srvBlocks = (signed char *)malloc(voxels);
    if (!srvBlocks) return;

    srvWX = hx; srvWY = hy; srvWZ = hz;
    const unsigned char *p = mapFileData + 12;
    for (int x = 0; x < hx; x++)
        for (int z = 0; z < hz; z++)
            for (int y = 0; y < hy; y++) {
                // (unsigned char)255 при записи AIR(-1) → signed char -1
                srvBlocks[SrvBlockIndex(x, y, z)] = (signed char)(*p++);
            }
    srvMapReady = 1;
    printf("Server block grid ready %dx%dx%d (%zu voxels) — wall occlusion ON\n",
           hx, hy, hz, voxels);
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
    const int maxSteps = srvWX + srvWY + srvWZ + 8;
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
static SOCKET clientSockets[MAX_SERVER_SLOTS];
/* clientConnected объявлен выше (рядом с clientRoom) */
static double clientLastSeen[MAX_SERVER_SLOTS];
static bool clientMapTransferring[MAX_SERVER_SLOTS];
/* >0 = для probe-соединения (LB_REQ/STATUS_REQ) запланировано мягкое
 * закрытие. Раньше сокет рвали closesocket() в тот же тик, что и отправку
 * ответа - на голом Linux это обычно ок, но соединение идёт через Railway
 * TCP proxy, и если апстрим-сокет закрывается раньше, чем прокси успевает
 * дорелеить последний пакет клиенту, "хвост" ответа теряется. Даём чуть
 * времени на фактическую доставку, слот освобождается по таймеру ниже
 * (или раньше, обычным путём, если клиент сам отключился). */
static double clientProbeCloseAt[MAX_SERVER_SLOTS];
static char clientRecvBuf[MAX_SERVER_SLOTS][RECV_STREAM_BUF];
static int clientRecvLen[MAX_SERVER_SLOTS];
static int clientCount = 0; /* верхняя граница занятых global-слотов (не «число игроков») */
static float clientSpawnProtect[MAX_SERVER_SLOTS];

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
static double lastStateSentAt = 0.0; // fallback STATE→SNAPSHOT
static int pongCount = 0;

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

static int CountConnectedPlayers(void) {
    int n = 0;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++)
        if (clientConnected[i]) n++;
    return n;
}

// Очки от локального бота клиента (килл/урон по вражескому боту или
// по игроку) — доверяем присланному значению points (клиент уже посчитал
// его по MATCH_PTS_BOT_KILL/MATCH_PTS_BOT_DAMAGE_DIV), но клэмпим сверху
// на случай мусора/читов в одном сообщении и не считаем очки вне PLAYING.
static void ApplyBotScoreForRoom(int room, int faction, int points) {
    if (room < 0 || room >= MAX_ROOMS) return;
    if (rooms[room].phase != MATCH_PLAYING) return;
    if (points <= 0) return;
    if (points > MATCH_PTS_KILL) points = MATCH_PTS_KILL;
    if (faction == 0) rooms[room].scoreShield += points; else rooms[room].scoreVolya += points;
}
static void ApplyBotScore(int faction, int points) {
    ApplyBotScoreForRoom(0, faction, points);
}
static void MatchStartPlayingRoom(int r) {
    if (r < 0 || r >= MAX_ROOMS) return;
    rooms[r].phase = MATCH_PLAYING;
    rooms[r].timeLeft = MATCH_ROUND_SEC;
    rooms[r].scoreShield = rooms[r].scoreVolya = 0;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (!clientConnected[i] || clientRoom[i] != r) continue;
        clientStates[i].health = (clientStates[i].faction == 0) ? 110.0f : 100.0f;
        clientStates[i].alive = true;
        clientSpawnProtect[i] = 3.0f;
        clientStates[i].spawnProtected = 1;
    }
    printf("Room %d: PLAYING (%d players)\n", r, CountRoomPlayers(r));
}
static void MatchTick(void) {
    double now = Net_Now();
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (!clientConnected[i]) continue;
        float dt_sp = 0.033f;
        if (clientSpawnProtect[i] > 0.0f) {
            clientSpawnProtect[i] -= dt_sp;
            if (clientSpawnProtect[i] < 0.0f) clientSpawnProtect[i] = 0.0f;
        }
        clientStates[i].spawnProtected = (clientSpawnProtect[i] > 0.0f) ? 1 : 0;
    }
    for (int r = 0; r < MAX_ROOMS; r++) {
        float dt = (rooms[r].lastTick > 0.0) ? (float)(now - rooms[r].lastTick) : 0.0f;
        if (dt < 0.f) dt = 0.f;
        if (dt > 0.25f) dt = 0.25f;
        rooms[r].lastTick = now;
        int n = CountRoomPlayers(r);
        if (n == 0) {
            rooms[r].phase = MATCH_WAITING;
            rooms[r].timeLeft = 0.f;
            rooms[r].scoreShield = rooms[r].scoreVolya = 0;
            continue;
        }
        if (rooms[r].phase == MATCH_WAITING) {
            rooms[r].timeLeft = 0.f;
            if (n >= 2) {
                rooms[r].phase = MATCH_COUNTDOWN;
                rooms[r].timeLeft = MATCH_WAIT_SEC;
                printf("Room %d: COUNTDOWN (%d players)\n", r, n);
            }
        } else if (rooms[r].phase == MATCH_COUNTDOWN) {
            if (n < 2) {
                rooms[r].phase = MATCH_WAITING;
                rooms[r].timeLeft = 0.f;
            } else {
                rooms[r].timeLeft -= dt;
                if (rooms[r].timeLeft <= 0.f) MatchStartPlayingRoom(r);
            }
        } else if (rooms[r].phase == MATCH_PLAYING) {
            rooms[r].timeLeft -= dt;
            if (rooms[r].scoreShield >= MATCH_SCORE_WIN || rooms[r].scoreVolya >= MATCH_SCORE_WIN ||
                rooms[r].timeLeft <= 0.f) {
                rooms[r].phase = MATCH_ENDED;
                rooms[r].timeLeft = MATCH_END_HOLD_SEC;
                printf("Room %d: ENDED S=%d V=%d\n", r, rooms[r].scoreShield, rooms[r].scoreVolya);
            }
        } else if (rooms[r].phase == MATCH_ENDED) {
            rooms[r].timeLeft -= dt;
            if (rooms[r].timeLeft <= 0.f) {
                rooms[r].phase = MATCH_WAITING;
                rooms[r].timeLeft = 0.f;
                rooms[r].scoreShield = rooms[r].scoreVolya = 0;
            }
        }
    }
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
#if !defined(_WIN32) && defined(TCP_QUICKACK)
    /* Linux: отключить delayed ACK — иначе +40..200мс к мелким пакетам (PING/STATE) */
    setsockopt(s, IPPROTO_TCP, TCP_QUICKACK, (const char*)&flag, sizeof(flag));
#endif
    /* Чуть больше буферов — меньше блокировок send при burst snapshot */
    int buf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
}

// Досылает данные целиком: send() на TCP может отправить меньше, чем
// попросили, если буфер сокета временно полон.
static bool SendAll(SOCKET s, const void *data, size_t len) {
    const char *p = (const char*)data;
    size_t sent = 0;
    int spins = 0;
    while (sent < len) {
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n > 0) { sent += (size_t)n; spins = 0; continue; }
#ifdef _WIN32
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (++spins > 200) return false; /* ~20мс max stall */
            Sleep(0); /* yield, не Sleep(1) */
            continue;
        }
#else
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            if (++spins > 200) return false;
            struct timespec ts = {0, 50 * 1000}; /* 0.05 мс */
            nanosleep(&ts, NULL);
            continue;
        }
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
    if (listen(listenSock, 64) < 0) {
        closesocket(listenSock); listenSock = INVALID_SOCKET; return false;
    }
    SetNonBlocking(listenSock);

    for (int i = 0; i < MAX_SERVER_SLOTS; i++) { clientSockets[i] = INVALID_SOCKET; clientConnected[i] = false; clientRoom[i] = -1; clientLocalId[i] = -1; }
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

static void Network_LoadSpawnCfg(void) {
    if (spawnCfgData) { free(spawnCfgData); spawnCfgData = NULL; }
    spawnCfgSize = 0;
    /* Сначала spawn_points.cfg рядом с бинарником (dedicated), иначе тот же путь */
    const char *paths[] = { NETWORK_SPAWN_LOCAL_PATH, NULL };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long sz = ftell(f);
        if (sz <= 0 || sz > SPAWN_CFG_MAX) { fclose(f); continue; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        spawnCfgData = (char *)malloc((size_t)sz + 1);
        if (!spawnCfgData) { fclose(f); continue; }
        if (fread(spawnCfgData, 1, (size_t)sz, f) != (size_t)sz) {
            free(spawnCfgData); spawnCfgData = NULL; fclose(f); continue;
        }
        fclose(f);
        spawnCfgData[sz] = '\0';
        spawnCfgSize = (uint32_t)sz;
        printf("Spawn cfg loaded for serving: %s (%u bytes)\n", paths[i], spawnCfgSize);
        return;
    }
    printf("No spawn_points.cfg on server — clients keep local\n");
}

static void SendSpawnCfg(SOCKET s) {
    if (!spawnCfgData || spawnCfgSize == 0) {
        /* пустой payload = у сервера нет своего cfg */
        SendFramed(s, MSG_SPAWN_CFG, NULL, 0);
        return;
    }
    SendFramed(s, MSG_SPAWN_CFG, spawnCfgData, spawnCfgSize);
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
    Network_LoadSpawnCfg();
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
    // Шлём чанками и чуть дышим между пачками — иначе Railway TCP Proxy
    // и неблокирующий сокет могут оборвать поток на 18 МБ.
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
        if ((offset / MAP_CHUNK_PAYLOAD) % 8 == 0) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 500 * 1000}; // 0.5 мс
            nanosleep(&ts, NULL);
#endif
        }
    }
    printf("Map fully sent (%u bytes)\n", mapFileSize);
}

/* Клиент: применить spawn_points.cfg с сервера (приоритет над локальным). */
static void ClientApplySpawnCfg(const char *data, uint32_t len) {
    if (!data || len == 0) {
        remove(NETWORK_SPAWN_SERVER_PATH);
        printf("Server has no spawn_points.cfg — using local if any\n");
        return;
    }
    FILE *out = fopen(NETWORK_SPAWN_SERVER_PATH, "wb");
    if (!out) {
        printf("Failed to write %s\n", NETWORK_SPAWN_SERVER_PATH);
        return;
    }
    fwrite(data, 1, len, out);
    fclose(out);
    printf("Server spawn_points.cfg saved (%u bytes) — priority over local\n", len);
#ifndef NETWORK_HEADLESS_BUILD
    extern void SpawnEdit_Load(void);
    SpawnEdit_Load();
#endif
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
            } else if (type == MSG_SPAWN_CFG) {
                ClientApplySpawnCfg(payload, len);
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
    if (!buf) {
        printf("Map download: out of memory (%u bytes)\n", remoteSize);
        return false;
    }
    // Таймаут: минимум 3 мин + ~30с на каждый МБ (Railway TCP бывает медленный)
    double timeoutSec = 180.0 + (double)remoteSize / (1024.0 * 1024.0) * 30.0;
    if (timeoutSec > 900.0) timeoutSec = 900.0;
    deadline = Net_Now() + timeoutSec;
    uint32_t received = 0;
    double lastUi = 0.0;
    double lastLog = 0.0;

    while (received < remoteSize && Net_Now() < deadline) {
        if (!PumpRecv(sock, recvStreamBuf, &recvStreamLen)) {
            printf("Map download: connection lost at %u/%u\n", received, remoteSize);
            free(buf);
            return false;
        }
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
                // Чанки идут по порядку — high-water = сколько байт подряд с начала
                if (offset <= received && offset + dataLen > received)
                    received = offset + dataLen;
                else if (offset + dataLen > received && offset == received)
                    received = offset + dataLen;
                else if (offset + dataLen > received)
                    received = offset + dataLen; // fallback if reordered
            } else if (type == MSG_WELCOME && len == sizeof(WelcomeMsg)) {
                WelcomeMsg w; memcpy(&w, payload, sizeof(w));
                myId = w.assignedId;
            } else if (type == MSG_SPAWN_CFG) {
                ClientApplySpawnCfg(payload, len);
            }
        }

        double now = Net_Now();
#ifndef NETWORK_HEADLESS_BUILD
        // Экран загрузки ~15 раз/сек. Кириллица — через uiFont (у default-шрифта raylib её нет → "????").
        if (now - lastUi > 0.066) {
            lastUi = now;
            float pct = remoteSize ? (100.0f * (float)received / (float)remoteSize) : 0.0f;
            int sw = GetScreenWidth(), sh = GetScreenHeight();
            extern Font uiFont;
            BeginDrawing();
            ClearBackground((Color){12, 14, 18, 255});
            const char *title = "Загрузка карты с сервера...";
            Vector2 ts = MeasureTextEx(uiFont, title, 28, 1);
            DrawTextEx(uiFont, title, (Vector2){sw/2 - ts.x/2, (float)(sh/2 - 60)}, 28, 1, WHITE);
            char line[128];
            snprintf(line, sizeof(line), "%.1f / %.1f MB  (%.0f%%)",
                     received / (1024.0f*1024.0f),
                     remoteSize / (1024.0f*1024.0f), pct);
            Vector2 ls = MeasureTextEx(uiFont, line, 20, 1);
            DrawTextEx(uiFont, line, (Vector2){sw/2 - ls.x/2, (float)(sh/2 - 20)}, 20, 1, LIGHTGRAY);
            int barW = sw / 2, barH = 18;
            int bx = sw/2 - barW/2, by = sh/2 + 20;
            DrawRectangle(bx, by, barW, barH, (Color){40, 40, 50, 255});
            int fill = (int)(barW * (pct / 100.0f));
            if (fill > 0) DrawRectangle(bx, by, fill, barH, (Color){80, 180, 120, 255});
            DrawRectangleLines(bx, by, barW, barH, WHITE);
            const char *hint = "Не закрывайте игру";
            Vector2 hs = MeasureTextEx(uiFont, hint, 16, 1);
            DrawTextEx(uiFont, hint, (Vector2){sw/2 - hs.x/2, (float)(by + 30)}, 16, 1, (Color){180, 180, 180, 255});
            EndDrawing();
        }
#endif
        if (now - lastLog > 2.0) {
            lastLog = now;
            printf("Map download progress: %u / %u (%.0f%%)\n",
                   received, remoteSize,
                   remoteSize ? 100.0 * received / remoteSize : 0.0);
        }
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec ts = {0, 1 * 1000 * 1000};
        nanosleep(&ts, NULL);
#endif
    }
    if (received < remoteSize) {
        printf("Map download incomplete (%u/%u) — timeout or stall\n", received, remoteSize);
        free(buf);
        return false;
    }
    uint32_t gotCrc = Crc32FileBuffer(buf, remoteSize);
    if (gotCrc != remoteCrc) {
        printf("Map CRC mismatch after download (got 0x%08X, want 0x%08X)\n", gotCrc, remoteCrc);
        free(buf);
        return false;
    }
    FILE *out = fopen(NETWORK_MAP_CACHE_PATH, "wb");
    if (!out) { free(buf); return false; }
    fwrite(buf, 1, remoteSize, out);
    fclose(out);
    free(buf);
    printf("Map saved to %s\n", NETWORK_MAP_CACHE_PATH);
    // Сообщаем серверу, что карта на месте — снимет clientMapTransferring
    SendFramed(sock, MSG_MAP_HAVE, NULL, 0);
    return true;
}

bool Network_InitServer(void) {
    Rooms_Init();
    myRoomId = 0;
    if (!StartListenSocket(SERVER_PORT)) return false;
    hostPlaysToo = true;
    myId = 0;
    clientCount = 1;
    clientStates[0] = (PlayerState){ .health = 100, .alive = true };
    clientConnected[0] = true;
    clientLastSeen[0] = Net_Now();
    clientRoom[0] = 0;
    clientLocalId[0] = 0;
    Network_LoadMapFile(NETWORK_MAP_HOST_PATH);
    printf("TCP listen-server started on port %d\n", SERVER_PORT);
    return true;
}

bool Network_InitDedicatedServer(int port) {
    Rooms_Init();
    if (!StartListenSocket(port)) return false;
    hostPlaysToo = false;
    myId = -1;      // у dedicated-сервера нет своего игрока
    clientCount = 0;
    Network_LoadMapFile(NETWORK_MAP_DEDICATED_PATH);
    /* если карты нет — всё равно подхватить spawn_points.cfg */
    if (!spawnCfgData) Network_LoadSpawnCfg();
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

bool Network_ProbeServerEx(const char *addr, int timeoutMs, float *outPingMs,
                           int *outPlayerCount, int *outMaxPlayers) {
    if (outPingMs) *outPingMs = 0.0f;
    if (outPlayerCount) *outPlayerCount = -1;
    if (outMaxPlayers) *outMaxPlayers = MAX_PLAYERS;
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
    if (!online) { closesocket(s); return false; }
    if (outPingMs) *outPingMs = (float)((t1 - t0) * 1000.0);

    SetNonBlocking(s);
    SetTcpNoDelay(s);
    /* Короткий STATUS_REQ — сервер ответит playerCount */
    SendFramed(s, MSG_STATUS_REQ, NULL, 0);
    char rbuf[512];
    int rlen = 0;
    double deadline = Net_Now() + (timeoutMs > 0 ? timeoutMs / 1000.0 : 0.7);
    while (Net_Now() < deadline) {
        if (!PumpRecv(s, rbuf, &rlen)) break;
        uint32_t type, len;
        char payload[BUFFER_SIZE];
        if (TryParseFrame(rbuf, &rlen, &type, payload, sizeof(payload), &len)) {
            if (type == MSG_STATUS && len == sizeof(ServerStatusMsg)) {
                ServerStatusMsg st;
                memcpy(&st, payload, sizeof(st));
                if (outPlayerCount) *outPlayerCount = st.playerCount;
                if (outMaxPlayers) *outMaxPlayers = st.maxPlayers > 0 ? st.maxPlayers : MAX_PLAYERS;
                break;
            }
            /* WELCOME/MAP_INFO от полноценного accept — игнор, продолжаем ждать STATUS */
        }
    }
    closesocket(s);
    return true;
}

void Network_SendProfileLogin(const char *name) {
    if (isServer || sock == INVALID_SOCKET) return;
    ProfileLoginMsg pl = {0};
    if (name && name[0]) {
        strncpy(pl.name, name, PROFILE_NAME_MAX - 1);
        pl.name[PROFILE_NAME_MAX - 1] = '\0';
    } else {
        strncpy(pl.name, "Player", PROFILE_NAME_MAX - 1);
    }
    SendFramed(sock, MSG_PROFILE_LOGIN, &pl, sizeof(pl));
}

bool Network_ReceiveProfileInfo(ProfileInfoMsg *out) {
    if (!hasPendingProfileInfo || !out) return false;
    *out = pendingProfileInfo;
    hasPendingProfileInfo = false;
    return true;
}

void Network_RequestLeaderboard(void) {
    if (isServer) {
        Profile_BuildLeaderboard(&cachedLeaderboard);
        hasCachedLeaderboard = true;
        return;
    }
    if (sock == INVALID_SOCKET) return;
    SendFramed(sock, MSG_LB_REQ, NULL, 0);
}

bool Network_ReceiveLeaderboard(LeaderboardMsg *out) {
    if (!hasCachedLeaderboard || !out) return false;
    *out = cachedLeaderboard;
    return true;
}

const LeaderboardMsg *Network_GetCachedLeaderboard(void) {
    return hasCachedLeaderboard ? &cachedLeaderboard : NULL;
}

bool Network_FetchLeaderboard(const char *addr, int timeoutMs, LeaderboardMsg *out) {
    if (!out || !addr) return false;
    memset(out, 0, sizeof(*out));
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
    bool online = ConnectWithTimeout(s, res, timeoutMs);
    freeaddrinfo(res);
    if (!online) { closesocket(s); return false; }

    SetNonBlocking(s);
    SetTcpNoDelay(s);
    SendFramed(s, MSG_LB_REQ, NULL, 0);

    char rbuf[8192];
    int rlen = 0;
    double deadline = Net_Now() + (timeoutMs > 0 ? timeoutMs / 1000.0 : 1.5);
    bool got = false;
    while (Net_Now() < deadline) {
        if (!PumpRecv(s, rbuf, &rlen)) break;
        uint32_t type, len;
        char payload[BUFFER_SIZE];
        if (!TryParseFrame(rbuf, &rlen, &type, payload, sizeof(payload), &len)) continue;
        if (type == MSG_LB_DATA && len == sizeof(LeaderboardMsg)) {
            memcpy(out, payload, sizeof(LeaderboardMsg));
            cachedLeaderboard = *out;
            hasCachedLeaderboard = true;
            got = true;
            break;
        }
    }
    closesocket(s);
    return got;
}

// ===================== Асинхронные probe-запросы =====================
// Network_FetchLeaderboard/Network_ProbeServerEx блокируют вызывающий поток
// на весь connect+recv (до timeoutMs*2 по факту - отдельный бюджет на коннект
// и на ожидание ответа). Вызванные прямо из кадра рендера (UI_DrawLeaderboard)
// они стопорят игру, а если это происходит во время реального матча - не
// пампится основной игровой сокет, что рискует довести до собственного
// разрыва по CLIENT_TIMEOUT. Ниже - версия без блокировки: Start() один раз,
// Poll() каждый кадр, весь connect/send/recv неблокирующий и размазан по кадрам.
// Коды состояния NET_ASYNC_* объявлены в network.h (общие с вызывающим кодом).

typedef struct {
    SOCKET sock;
    int stage; // 0 = нет активного запроса, 1 = ждём connect(), 2 = ждём ответ
    double deadline;
    char rbuf[8192];
    int rlen;
    uint32_t reqType;
    uint32_t wantType;
    uint32_t wantLen; // 0 = любая длина
    char resultPayload[BUFFER_SIZE];
} AsyncNetReq;

static void AsyncReq_Close(AsyncNetReq *r) {
    if (r->sock != INVALID_SOCKET) { closesocket(r->sock); r->sock = INVALID_SOCKET; }
    r->stage = 0;
}

static bool AsyncReq_ParseHostPort(const char *addr, char *host, size_t hostSz, int *outPort) {
    int port = SERVER_PORT;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t hostLen = (size_t)(colon - addr);
        if (hostLen >= hostSz) hostLen = hostSz - 1;
        memcpy(host, addr, hostLen);
        host[hostLen] = '\0';
        int p = atoi(colon + 1);
        if (p > 0) port = p;
    } else {
        snprintf(host, hostSz, "%s", addr);
    }
    *outPort = port;
    return true;
}

// Запускает новый неблокирующий запрос (обрывает предыдущий, если ещё не завершён).
static void AsyncReq_Start(AsyncNetReq *r, const char *addr, uint32_t reqType,
                            uint32_t wantType, uint32_t wantLen, int timeoutMs) {
    AsyncReq_Close(r);
    if (!addr || !InitSockets()) return;

    char host[128]; int port;
    AsyncReq_ParseHostPort(addr, host, sizeof(host), &port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    // getaddrinfo() сам по себе блокирующий (DNS) и не покрыт timeoutMs -
    // на мобильной сети иногда даёт основную часть задержки. Отдельного
    // неблокирующего резолвера тут нет, но это не более 8К символов кода
    // ради разовой асинхронности DNS; дальнейший connect/recv уже честно
    // размазаны по кадрам.
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return;

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return; }
    SetNonBlocking(s);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    bool inProgress;
#ifdef _WIN32
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return; }
#else
    if (rc != 0 && errno != EINPROGRESS) { closesocket(s); return; }
#endif
    inProgress = (rc != 0);

    r->sock = s;
    r->reqType = reqType;
    r->wantType = wantType;
    r->wantLen = wantLen;
    r->rlen = 0;
    r->deadline = Net_Now() + (timeoutMs > 0 ? timeoutMs / 1000.0 : 2.0);
    if (inProgress) {
        r->stage = 1;
    } else {
        // мгновенный коннект (localhost и т.п.) - шлём запрос сразу
        SetTcpNoDelay(s);
        SendFramed(s, reqType, NULL, 0);
        r->stage = 2;
    }
}

// 0 = ещё ждём, 1 = получили нужный ответ (resultPayload заполнен), -1 = провал/таймаут
static int AsyncReq_Poll(AsyncNetReq *r) {
    if (r->stage == 0) return -1;
    if (Net_Now() > r->deadline) { AsyncReq_Close(r); return -1; }

    if (r->stage == 1) {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(r->sock, &wf);
        FD_SET(r->sock, &ef);
        struct timeval tv = {0, 0}; // неблокирующий опрос — не ждём внутри select()
        int sel = select((int)r->sock + 1, NULL, &wf, &ef, &tv);
        if (sel > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(r->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
            if (err != 0 || FD_ISSET(r->sock, &ef)) { AsyncReq_Close(r); return -1; }
            if (FD_ISSET(r->sock, &wf)) {
                SetTcpNoDelay(r->sock);
                SendFramed(r->sock, r->reqType, NULL, 0);
                r->stage = 2;
            }
        }
        return 0;
    }

    // stage == 2: ждём ответ
    if (!PumpRecv(r->sock, r->rbuf, &r->rlen)) { AsyncReq_Close(r); return -1; }
    uint32_t type, len;
    while (TryParseFrame(r->rbuf, &r->rlen, &type, r->resultPayload, sizeof(r->resultPayload), &len)) {
        if (type == r->wantType && (r->wantLen == 0 || len == r->wantLen)) {
            AsyncReq_Close(r);
            return 1;
        }
        // WELCOME/MAP_INFO от полноценного accept - игнор, ждём нужный тип дальше
    }
    return 0;
}

static AsyncNetReq g_lbAsyncReq = { .sock = INVALID_SOCKET };
static bool g_lbAsyncInFlight = false;

void Network_LeaderboardFetchAsync_Start(const char *addr, int timeoutMs) {
    AsyncReq_Start(&g_lbAsyncReq, addr, MSG_LB_REQ, MSG_LB_DATA, sizeof(LeaderboardMsg), timeoutMs);
    g_lbAsyncInFlight = (g_lbAsyncReq.stage != 0);
}

int Network_LeaderboardFetchAsync_Poll(LeaderboardMsg *out) {
    if (!g_lbAsyncInFlight) return NET_ASYNC_IDLE;
    int r = AsyncReq_Poll(&g_lbAsyncReq);
    if (r == 0) return NET_ASYNC_PENDING;
    g_lbAsyncInFlight = false;
    if (r == 1 && out) {
        memcpy(out, g_lbAsyncReq.resultPayload, sizeof(LeaderboardMsg));
        cachedLeaderboard = *out;
        hasCachedLeaderboard = true;
        return NET_ASYNC_DONE;
    }
    return NET_ASYNC_FAILED;
}

static AsyncNetReq g_stAsyncReq = { .sock = INVALID_SOCKET };
static bool g_stAsyncInFlight = false;

void Network_StatusFetchAsync_Start(const char *addr, int timeoutMs) {
    AsyncReq_Start(&g_stAsyncReq, addr, MSG_STATUS_REQ, MSG_STATUS, sizeof(ServerStatusMsg), timeoutMs);
    g_stAsyncInFlight = (g_stAsyncReq.stage != 0);
}

int Network_StatusFetchAsync_Poll(int *outPlayerCount, int *outMaxPlayers) {
    if (!g_stAsyncInFlight) return NET_ASYNC_IDLE;
    int r = AsyncReq_Poll(&g_stAsyncReq);
    if (r == 0) return NET_ASYNC_PENDING;
    g_stAsyncInFlight = false;
    if (r == 1) {
        ServerStatusMsg st;
        memcpy(&st, g_stAsyncReq.resultPayload, sizeof(st));
        if (outPlayerCount) *outPlayerCount = st.playerCount;
        if (outMaxPlayers) *outMaxPlayers = st.maxPlayers > 0 ? st.maxPlayers : MAX_PLAYERS;
        return NET_ASYNC_DONE;
    }
    return NET_ASYNC_FAILED;
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
        } else {
            clientSpawnProtect[0] = 3.0f;
        }
        clientStates[0].spawnProtected = (clientSpawnProtect[0] > 0.0f) ? 1 : 0;
    } else {
        if (sock == INVALID_SOCKET) return;
        lastStateSentAt = Net_Now();
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
    if (i < 0 || i >= MAX_SERVER_SLOTS) return;
    if (clientSockets[i] != INVALID_SOCKET) {
        closesocket(clientSockets[i]);
        clientSockets[i] = INVALID_SOCKET;
    }
    clientConnected[i] = false;
    clientMapTransferring[i] = false;
    clientStates[i] = (PlayerState){0};
    clientRecvLen[i] = 0;
    clientNames[i][0] = '\0';
    clientProfileIdx[i] = -1;
    clientRoom[i] = -1;
    clientLocalId[i] = -1;
    clientProbeCloseAt[i] = 0.0;
    rosterDirty = true;
    Rooms_TrimEmpty();
}

static void ServerCheckTimeouts(void) {
    double now = Net_Now();
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (hostPlaysToo && i == 0) continue;
        if (!clientConnected[i]) continue;
        // Мягкое закрытие probe-соединений (LB_REQ/STATUS_REQ) - см.
        // clientProbeCloseAt: ответ уже отправлен, ждём, дадим сети время
        // фактически его доставить перед закрытием сокета.
        if (clientProbeCloseAt[i] > 0.0 && now > clientProbeCloseAt[i]) {
            ClearClientSlot(i);
            continue;
        }
        // Пока клиент качает карту (~18 МБ), STATE не шлёт — таймаут 5с убивал
        // соединение на последних килобайтах (см. "connection lost at 18370560/...").
        if (clientMapTransferring[i]) continue;
        if (now - clientLastSeen[i] > CLIENT_TIMEOUT) {
            printf("Client %d timed out, disconnecting\n", i);
            ClearClientSlot(i);
        }
    }
}

void Network_BroadcastHitConfirm(HitConfirm confirm) {
    if (!isServer) return;
    int room = lastHitRoom;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (!clientConnected[i] || clientRoom[i] != room) continue;
        if (hostPlaysToo && i == 0) continue; /* host gets local path */
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

    int room = (event.shooterId >= 0 && event.shooterId < MAX_SERVER_SLOTS)
        ? clientRoom[event.shooterId] : -1;
    if (room < 0) return;
    lastHitRoom = room;

    // Стена по серверной карте. Если карты нет — occlusion выключен.
    float wallDist = ServerBlockRaycast(event.origin, event.direction, HITSCAN_RANGE);

    // Один хитбокс на всю модель (как на клиенте): центр торса, радиус ~половина роста.
    const float kEyeH = 1.65f;
    const float kPlayerH = 1.8f;
    const float kRadius = kPlayerH * 0.5f; // ~0.9 — покрывает ноги→голову

    float bestDist = HITSCAN_RANGE;
    int bestTarget = -1;
    for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
        if (i == event.shooterId || !clientConnected[i] || !clientStates[i].alive) continue;
        if (clientRoom[i] != room) continue;
        if (clientSpawnProtect[i] > 0.0f || clientStates[i].spawnProtected)
            continue; /* защита спавна */
        /* friendly fire OFF */
        if (clientStates[i].faction == clientStates[event.shooterId].faction) continue;
        Vector3 eye = clientStates[i].position;
        Vector3 center = { eye.x, eye.y - kEyeH + kPlayerH * 0.5f, eye.z };
        Vector3 oc = Vector3Subtract(event.origin, center);
        float b = Vector3DotProduct(oc, event.direction);
        float c = Vector3DotProduct(oc, oc) - kRadius * kRadius;
        float disc = b * b - c;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t < 0) t = -b + sqrtf(disc);
        if (t > 0.0f && t < bestDist && t < wallDist) {
            bestDist = t;
            bestTarget = i;
        }
    }

    HitConfirm confirm;
    confirm.shooterId = clientLocalId[event.shooterId];
    if (bestTarget != -1) {
        // Базовый урон оружия + спад с дистанцией (как на клиенте)
        float base = (event.weaponType == 0) ? 25.0f : 35.0f;
        float mul;
        if (bestDist < 5.0f)       mul = 1.20f;
        else if (bestDist < 18.0f) mul = 1.00f;
        else if (bestDist < 45.0f) mul = 1.00f - (bestDist - 18.0f) / 27.0f * 0.55f;
        else                       mul = 0.40f;
        float dmgF = base * mul;
        /* Воля в движении — бонус «фланг» (+10% урона) */
        if (clientStates[event.shooterId].faction == 1 /* VOLYA */ &&
            clientStates[event.shooterId].speed >= 2.5f) {
            dmgF *= 1.10f;
        }
        /* Щит — броня: −15% входящего урона */
        if (clientStates[bestTarget].faction == 0 /* SHIELD */) {
            dmgF *= 0.85f;
        }
        int damage = (int)(dmgF + 0.5f);
        if (damage < 1) damage = 1;
        // Если клиент прислал свой расчёт — слегка учитываем, но авторитет у сервера
        if (event.damage > 0.0f) {
            int fromClient = (int)(event.damage + 0.5f);
            if (fromClient < 1) fromClient = 1;
            // Берём серверный falloff (защита от читов), клиентский только как подсказка
            (void)fromClient;
        }
        // Очки только в PLAYING
        if (rooms[room].phase == MATCH_PLAYING) {
            int pts = damage / MATCH_PTS_DAMAGE_DIV;
            if (pts < 1) pts = 1;
            int sf = clientStates[event.shooterId].faction;
            if (sf == 0) rooms[room].scoreShield += pts; else rooms[room].scoreVolya += pts;
        }
        clientStates[bestTarget].health -= damage;
        /* Потолок HP по фракции (Щит 110, Воля 100) — на случай хила/десинка */
        {
            float maxHp = (clientStates[bestTarget].faction == 0) ? 110.0f : 100.0f;
            if (clientStates[bestTarget].health > maxHp)
                clientStates[bestTarget].health = maxHp;
        }
        if (clientStates[bestTarget].health <= 0) {
            clientStates[bestTarget].alive = false;
            clientStates[bestTarget].health = 0;
            if (rooms[room].phase == MATCH_PLAYING) {
                int sf = clientStates[event.shooterId].faction;
                if (sf == 0) rooms[room].scoreShield += MATCH_PTS_KILL; else rooms[room].scoreVolya += MATCH_PTS_KILL;
                Profile_AddKill(event.shooterId);
            }
            if (rooms[room].phase == MATCH_WAITING || rooms[room].phase == MATCH_COUNTDOWN) {
                clientStates[bestTarget].health = 100.0f;
                clientStates[bestTarget].alive = true;
            }
        }
        confirm.targetId = (bestTarget >= 0) ? clientLocalId[bestTarget] : -1;
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

        int room = PickRoomForJoin();
        if (room < 0) {
            printf("Server full (all rooms)\n");
            closesocket(c);
            continue;
        }
        int idx = AllocSlotInRoom(room);
        if (idx < 0) {
            closesocket(c);
            continue;
        }

        clientSockets[idx] = c;
        clientConnected[idx] = true;
        clientStates[idx] = (PlayerState){0};
        clientRecvLen[idx] = 0;
        clientLastSeen[idx] = Net_Now();
        clientNames[idx][0] = '\0';
        clientProfileIdx[idx] = -1;
        {
            const char *ipStr = inet_ntoa(from.sin_addr);
            strncpy(clientIP[idx], ipStr ? ipStr : "", sizeof(clientIP[idx]) - 1);
            clientIP[idx][sizeof(clientIP[idx]) - 1] = '\0';
        }
        clientRoom[idx] = room;
        clientLocalId[idx] = idx % MAX_PLAYERS;
        if (idx + 1 > clientCount) clientCount = idx + 1;
        rosterDirty = true;

        WelcomeMsg w = { .assignedId = clientLocalId[idx] };
        SendFramed(c, MSG_WELCOME, &w, sizeof(w));
        SendMapInfo(c);
        SendSpawnCfg(c);
        printf("Client id=%d room=%d local=%d (room players %d)\n",
               idx, room, clientLocalId[idx], CountRoomPlayers(room));
    }
}

bool Network_ReceiveSnapshot(GameSnapshot *snap) {
    if (isServer) {
        if (listenSock == INVALID_SOCKET) return false;
        bool gotAny = false;

        AcceptNewConnections();

        for (int i = 0; i < MAX_SERVER_SLOTS; i++) {
            if (hostPlaysToo && i == 0) continue;
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
                    } else {
                        clientSpawnProtect[i] = 3.0f;
                    }
                    clientStates[i].spawnProtected = (clientSpawnProtect[i] > 0.0f) ? 1 : 0;
                    /* автобаланс команд в руме */
                    {
                        int room = clientRoom[i];
                        if (room >= 0 && room < MAX_ROOMS) {
                            int want = clientStates[i].faction;
                            /* временно убрать себя из подсчёта */
                            int savedFac = clientStates[i].faction;
                            clientStates[i].faction = -1;
                            int bal = BalanceFaction(room, want);
                            clientStates[i].faction = bal;
                            if (bal != savedFac && want == savedFac)
                                printf("Autobalance: slot %d room %d %d->%d\n", i, room, savedFac, bal);
                        }
                    }
                    clientMapTransferring[i] = false;
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
                } else if (type == MSG_PING && len == 8) {
                    // Эхо 8 байт timestamp (uint64 ms) — клиент сам считает RTT.
                    SendFramed(cs, MSG_PONG, payload, len);
                } else if (type == MSG_MAP_NEED) {
                    printf("Client %d requested map download\n", i);
                    clientMapTransferring[i] = true;
                    clientLastSeen[i] = Net_Now();
                    SendMapChunks(cs);
                    clientLastSeen[i] = Net_Now();
                    // Флаг снимем по MSG_MAP_HAVE или первому STATE
                } else if (type == MSG_MAP_HAVE) {
                    printf("Client %d has matching map cache\n", i);
                    clientMapTransferring[i] = false;
                    clientLastSeen[i] = Net_Now();
                } else if (type == MSG_BOTSCORE && len == sizeof(BotScoreEvent)) {
                    BotScoreEvent bse;
                    memcpy(&bse, payload, sizeof(bse));
                    ApplyBotScoreForRoom(clientRoom[i], bse.faction, bse.points);
                    gotAny = true;
                } else if (type == MSG_STATUS_REQ) {
                    ServerStatusMsg st;
                    /* Не считаем сам probe-сокет в playerCount */
                    st.playerCount = CountConnectedPlayers();
                    if (st.playerCount > 0) st.playerCount -= 1;
                    if (st.playerCount < 0) st.playerCount = 0;
                    st.maxPlayers = MAX_SERVER_SLOTS;
                    st.matchPhase = rooms[0].phase;
                    SendFramed(cs, MSG_STATUS, &st, sizeof(st));
                    /* Probe: слот всё равно не держим долго, но не рвём
                     * сокет в этот же тик - см. clientProbeCloseAt выше. */
                    if (!clientNames[i][0]) {
                        clientProbeCloseAt[i] = Net_Now() + 0.5;
                    }
                } else if (type == MSG_PROFILE_LOGIN && len == sizeof(ProfileLoginMsg)) {
                    ProfileLoginMsg pl;
                    memcpy(&pl, payload, sizeof(pl));
                    pl.name[PROFILE_NAME_MAX - 1] = '\0';
                    const char *wantName = pl.name[0] ? pl.name : "Player";

                    /* Ник уже активен у другого подключённого игрока — не пускаем. */
                    bool nameTaken = false;
                    for (int j = 0; j < MAX_SERVER_SLOTS; j++) {
                        if (j == i || !clientConnected[j]) continue;
                        if (clientNames[j][0] && strcmp(clientNames[j], wantName) == 0) {
                            nameTaken = true;
                            break;
                        }
                    }

                    ProfileInfoMsg info = {0};
                    if (nameTaken) {
                        info.rejected = 1;
                        clientProfileIdx[i] = -1;
                        printf("Profile login slot %d REJECTED: nick '%s' already in play\n", i, wantName);
                    } else {
                        bool limited = false;
                        int pidx = Profile_GetOrCreate(wantName, clientIP[i], &limited);
                        if (pidx < 0 && limited) {
                            info.rejected = 2;
                            clientProfileIdx[i] = -1;
                            printf("Profile login slot %d REJECTED: IP %s hit %d-account limit\n",
                                   i, clientIP[i], MAX_ACCOUNTS_PER_IP);
                        } else {
                            strncpy(clientNames[i], wantName, PROFILE_NAME_MAX - 1);
                            clientNames[i][PROFILE_NAME_MAX - 1] = '\0';
                            clientProfileIdx[i] = pidx;
                            if (pidx >= 0) {
                                info.tokens = g_profiles[pidx].tokens;
                                info.kills = g_profiles[pidx].kills;
                                info.rank = Profile_RankOf(pidx);
                            }
                            printf("Profile login slot %d: %s (kills=%d tokens=%d rank=%d)\n",
                                   i, clientNames[i], info.kills, info.tokens, info.rank);
                        }
                    }
                    SendFramed(cs, MSG_PROFILE_INFO, &info, sizeof(info));
                    if (info.rejected) {
                        /* Короткая жизнь слота — клиент успеет получить info, потом рвём. */
                        clientProbeCloseAt[i] = Net_Now() + 0.5;
                    }
                } else if (type == MSG_LB_REQ) {
                    LeaderboardMsg lb;
                    Profile_BuildLeaderboard(&lb);
                    SendFramed(cs, MSG_LB_DATA, &lb, sizeof(lb));
                    /* Короткий fetch без профиля — слот не держим долго,
                     * но не рвём сокет в этот же тик - см. clientProbeCloseAt. */
                    if (!clientNames[i][0]) {
                        clientProbeCloseAt[i] = Net_Now() + 0.5;
                    }
                }
            }
        }

        ServerCheckTimeouts();
        MatchTick();
        /* snap для host (рум 0); Broadcast шлёт каждому руму свой */
        {
            int r = hostPlaysToo ? 0 : 0;
            memset(snap, 0, sizeof(*snap));
            snap->matchPhase = rooms[r].phase;
            snap->scoreShield = rooms[r].scoreShield;
            snap->scoreVolya = rooms[r].scoreVolya;
            snap->timeLeft = rooms[r].timeLeft;
            int n = 0;
            for (int local = 0; local < MAX_PLAYERS; local++) {
                int g = r * MAX_PLAYERS + local;
                if (!clientConnected[g]) continue;
                snap->players[local] = clientStates[g];
                if (local + 1 > n) n = local + 1;
            }
            snap->playerCount = n;
        }
        // Обязательно вернуть true при смене состава, иначе disconnect
        // не дойдёт до клиентов и моделька «зависнет» на месте выхода.
        bool dirty = gotAny || rosterDirty || true; // матч-таймер всегда тикает
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
                clientMatchPhase = snap->matchPhase;
                clientScoreShield = snap->scoreShield;
                clientScoreVolya = snap->scoreVolya;
                clientTimeLeft = snap->timeLeft;
                got = true;
            } else if (type == MSG_HITCONFIRM && len == sizeof(HitConfirm)) {
                if (pendingConfirmCount < MAX_PACKETS_PER_TICK) {
                    memcpy(&pendingConfirms[pendingConfirmCount++], payload, sizeof(HitConfirm));
                }
            } else if (type == MSG_PROFILE_INFO && len == sizeof(ProfileInfoMsg)) {
                memcpy(&pendingProfileInfo, payload, sizeof(ProfileInfoMsg));
                hasPendingProfileInfo = true;
            } else if (type == MSG_LB_DATA && len == sizeof(LeaderboardMsg)) {
                memcpy(&cachedLeaderboard, payload, sizeof(LeaderboardMsg));
                hasCachedLeaderboard = true;
            } else if (type == MSG_PONG && len >= 8) {
                // payload = uint64_t миллисекунды (монотонные)
                uint64_t sentMs = 0;
                memcpy(&sentMs, payload, 8);
                double nowMs = Net_Now() * 1000.0;
                float rtt = (float)(nowMs - (double)sentMs);
                if (rtt < 0.0f) rtt = 0.0f;
                if (rtt > 60000.0f) rtt = 60000.0f;
                /* EMA: сглаживаем выбросы через Railway proxy */
                if (currentPingMs <= 0.1f) currentPingMs = rtt;
                else currentPingMs = currentPingMs * 0.7f + rtt * 0.3f;
                pingWaiting = false;
                if (pongCount++ < 5)
                    printf("Pong RTT=%.1f ms (ema=%.1f)\n", rtt, currentPingMs);
            }
        }

        // Fallback / уточнение: STATE→SNAPSHOT (работает даже без PONG)
        if (got && lastStateSentAt > 0.0) {
            float approx = (float)((Net_Now() - lastStateSentAt) * 1000.0);
            if (approx >= 0.0f && approx < 60000.0f) {
                if (pongCount == 0) {
                    // нет PONG — ведём сглаженную оценку
                    if (currentPingMs <= 1.0f) currentPingMs = approx;
                    else currentPingMs = currentPingMs * 0.75f + approx * 0.25f;
                }
                // если PONG уже есть — его значение приоритетнее, не затираем
            }
            lastStateSentAt = 0.0; // один замер на один STATE
        }

        // RTT до dedicated/listen-сервера. Таймаут 3с — если PONG потерялся,
        // пробуем снова (не зависаем на pingWaiting навсегда).
        double now = Net_Now();
        if (pingWaiting && (now - lastPingSentAt) > 3.0) {
            pingWaiting = false;
        }
        if (!pingWaiting && (now - lastPingSentAt) > 0.5) {
            uint64_t sentMs = (uint64_t)(now * 1000.0);
            if (SendFramed(sock, MSG_PING, &sentMs, 8)) {
                lastPingSentAt = now;
                pingWaiting = true;
            }
        }

        return got;
    }
}

void Network_BroadcastSnapshot(GameSnapshot snap) {
    (void)snap;
    if (!isServer) return;
    for (int r = 0; r < MAX_ROOMS; r++) {
        GameSnapshot out;
        memset(&out, 0, sizeof(out));
        out.matchPhase = rooms[r].phase;
        out.scoreShield = rooms[r].scoreShield;
        out.scoreVolya = rooms[r].scoreVolya;
        out.timeLeft = rooms[r].timeLeft;
        int n = 0;
        /* pack by local id */
        for (int local = 0; local < MAX_PLAYERS; local++) {
            int g = r * MAX_PLAYERS + local;
            if (!clientConnected[g]) continue;
            out.players[local] = clientStates[g];
            if (local + 1 > n) n = local + 1;
        }
        out.playerCount = n > 0 ? n : CountRoomPlayers(r);
        if (CountRoomPlayers(r) == 0) continue;
        for (int local = 0; local < MAX_PLAYERS; local++) {
            int g = r * MAX_PLAYERS + local;
            if (!clientConnected[g]) continue;
            if (hostPlaysToo && g == 0) continue;
            SendFramed(clientSockets[g], MSG_SNAPSHOT, &out, sizeof(out));
        }
    }
}

void Network_SendHitEvent(HitEvent event) {
    if (sock == INVALID_SOCKET || isServer) return;
    SendFramed(sock, MSG_HITEVENT, &event, sizeof(event));
}

void Network_SendBotScore(int faction, int points) {
    if (points <= 0) return;
    if (faction != 0 && faction != 1) return;
    if (isServer) {
        // Хост сам себе по сокету не отправит (см. Network_HostFire) —
        // применяем сразу к авторитетному счёту.
        ApplyBotScore(faction, points);
    } else {
        if (sock == INVALID_SOCKET) return;
        BotScoreEvent bse = { faction, points };
        SendFramed(sock, MSG_BOTSCORE, &bse, sizeof(bse));
    }
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

void Network_SeedPing(float ms) {
    if (ms < 0.0f) ms = 0.0f;
    if (ms > 60000.0f) ms = 60000.0f;
    currentPingMs = ms;
}

void Network_Close(void) {
    if (isServer) {
        int base = hostPlaysToo ? 1 : 0;
        for (int i = base; i < MAX_SERVER_SLOTS; i++) {
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
