#pragma once
#include <stdbool.h>
#include <stdint.h>

// Обычный графический клиент собирается с raylib как раньше.
// Headless dedicated-server собирается с -DNETWORK_HEADLESS_BUILD и
// вообще не тянет raylib (а значит не тянет GL/X11/ALSA в Docker-образ) —
// ему нужны только Vector3 и пара функций векторной математики,
// которые заменяет net_math.h.
#ifdef NETWORK_HEADLESS_BUILD
    #include "net_math.h"
#else
    #include "raylib.h"
#endif

#define MAX_PLAYERS 8              // 4v4 в одном руме
#define MAX_ROOMS 3                // скрытые параллельные матчи на dedicated
#define MAX_SERVER_SLOTS (MAX_PLAYERS * MAX_ROOMS) /* 24 слота на сервере */
#define SERVER_PORT 50000          // порт по умолчанию (он же "Internal Port" в Railway TCP Proxy)
#define BUFFER_SIZE 4096
#define CLIENT_TIMEOUT 5.0f
#define MAX_PACKETS_PER_TICK 64
#define HITSCAN_RANGE 100.0f       // единая дальность хитскана для клиента и сервера

/* ---- Режим: Team Deathmatch (Щит vs Воля) ----
 * Победа: набор MATCH_SCORE_WIN очков ИЛИ конец таймера (больше очков).
 * Убийство = MATCH_PTS_KILL, урон даёт мелкие очки. */
#define MATCH_SCORE_WIN      150
#define MATCH_ROUND_SEC      600.0f   /* 10 минут на раунд */
#define MATCH_WAIT_SEC       20.0f    /* countdown перед стартом */
#define MATCH_END_HOLD_SEC   15.0f    /* экран итога, потом новый цикл */
#define MATCH_PTS_KILL       15
#define MATCH_PTS_DAMAGE_DIV 5

/* ---- Очки от ботов (сервер тоже это резолвит, честно и одинаково для
 * всех клиентов) — заметно меньше, чем обычный ПвП-килл/урон выше:
 * килл ботом = 5 очков (против 15 за обычный килл), урон ботом делится
 * на больший делитель (15 против 5), т.е. очков за тот же урон меньше в 3 раза. */
#define MATCH_PTS_BOT_KILL       5
#define MATCH_PTS_BOT_DAMAGE_DIV 15

typedef enum {
    MATCH_WAITING   = 0,
    MATCH_COUNTDOWN = 1,
    MATCH_PLAYING   = 2,
    MATCH_ENDED     = 3
} MatchPhase;

typedef struct {
    Vector3 position;
    float yaw, pitch;
    int currentWeapon;
    bool firing;
    bool reloading;
    float health;
    bool alive;
    int faction;
    float speed;
    /* 1 = неуязвим после респавна (сервер не считает урон) */
    unsigned char spawnProtected;
} PlayerState;

typedef struct {
    PlayerState players[MAX_PLAYERS];
    int playerCount;
    int matchPhase;
    int scoreShield;
    int scoreVolya;
    float timeLeft;
} GameSnapshot;

typedef struct {
    int assignedId;
} WelcomeMsg;

typedef struct {
    Vector3 origin;
    Vector3 direction;
    int weaponType;
    int shooterId;
    float damage;
    bool headshot;
} HitEvent;

typedef struct {
    int targetId;
    int damage;
    int shooterId;
} HitConfirm;

/* Клиент -> сервер: локальный бот убил/ранил бота вражеской фракции или
 * локального игрока. points уже посчитаны клиентом по формулам
 * MATCH_PTS_BOT_KILL / MATCH_PTS_BOT_DAMAGE_DIV — сервер лишь добавляет их
 * к счёту нужной фракции (см. Network_SendBotScore), так что итоговый счёт
 * одинаковый у всех, как и обычный счёт по игрокам. */
typedef struct {
    int faction; // 0 = Щит, 1 = Воля — фракция бота-стрелка
    int points;
} BotScoreEvent;

/* ---- Профили / Топ-100 (жетоны и киллы на сервере) ---- */
#define PROFILE_NAME_MAX 40
#define LEADERBOARD_TOP 100

typedef struct {
    char name[PROFILE_NAME_MAX]; /* «Имя Кличка» UTF-8 */
} ProfileLoginMsg;

typedef struct {
    int tokens;
    int kills;
    int rank; /* 1..N, 0 если вне топа */
} ProfileInfoMsg;

typedef struct {
    char name[PROFILE_NAME_MAX];
    int kills;
    int tokens;
} LeaderboardEntry;

typedef struct {
    int count;
    LeaderboardEntry entries[LEADERBOARD_TOP];
} LeaderboardMsg;

typedef struct {
    int playerCount;
    int maxPlayers;
    int matchPhase;
} ServerStatusMsg;

extern bool isServer;

// Транспорт — TCP (раньше был UDP; сырой UDP Railway не пропускает наружу,
// TCP же можно опубликовать через Railway TCP Proxy). Формат payload-структур
// (PlayerState/HitEvent/...) не менялся, поменялась только упаковка "на проводе"
// (см. network.c — там теперь заголовок type+length перед каждым сообщением).
bool Network_InitServer(void);                 // listen-server: хост = игрок 0 (как раньше)
bool Network_InitClient(const char *serverAddr); // "host" или "host:port" (для Railway - host:port)
void Network_SendState(PlayerState state);
bool Network_ReceiveSnapshot(GameSnapshot *snap);
void Network_BroadcastSnapshot(GameSnapshot snap);
void Network_SendHitEvent(HitEvent event);
bool Network_ReceiveHitEvent(HitEvent *event);
void Network_BroadcastHitConfirm(HitConfirm confirm);
bool Network_ReceiveHitConfirm(HitConfirm *confirm);
// Очки за бота: работает и для клиента (шлёт по сети), и для хоста
// (тот же трюк, что и Network_HostFire — сам себе по сокету не отправишь,
// поэтому при isServer==true очки применяются к авторитетному счёту напрямую).
void Network_SendBotScore(int faction, int points);
void Network_Close(void);
int  Network_GetMyId(void);
float Network_GetPing(void);
void Network_SeedPing(float ms); // начальный пинг из probe списка серверов
bool Network_ProbeServer(const char *addr, int timeoutMs, float *outPingMs);
/* Расширенный probe: пинг + число игроков (если сервер отвечает MSG_STATUS) */
bool Network_ProbeServerEx(const char *addr, int timeoutMs, float *outPingMs,
                           int *outPlayerCount, int *outMaxPlayers);

/* Профиль: после коннекта клиент шлёт ник → сервер отвечает tokens/kills/rank */
void Network_SendProfileLogin(const char *name);
bool Network_ReceiveProfileInfo(ProfileInfoMsg *out);
/* Запрос/получение Топ-100 (можно без полного гейм-коннекта через probe-сессию
 * или после коннекта). Для UI используем кэш последнего ответа. */
void Network_RequestLeaderboard(void);
bool Network_ReceiveLeaderboard(LeaderboardMsg *out);
const LeaderboardMsg *Network_GetCachedLeaderboard(void);
/* Принудительно подтянуть лидерборд с официального/указанного адреса (короткая TCP-сессия) */
bool Network_FetchLeaderboard(const char *addr, int timeoutMs, LeaderboardMsg *out);

// Хост не проходит обычный цикл "отправил по сети -> получил из сокета",
// поэтому свои же попадания и свой же авторитетный health/alive
// ему нужно получать напрямую, в обход сокета. Используется только
// в listen-server режиме (Network_InitServer); для dedicated-сервера
// не актуально, там локального игрока нет.
void Network_HostFire(HitEvent event);              // резолвит собственный выстрел хоста локально
PlayerState Network_GetSelfAuthoritativeState(void); // clientStates[0], валидно только когда isServer

// Настоящий выделенный сервер: без локального игрока, слот 0 - обычный
// клиентский слот. Используется в server_main.c для деплоя на Railway.
bool Network_InitDedicatedServer(int port);

// ---- Карта сервера ----
// Клиент после коннекта скачивает/проверяет карту (CRC32). Кэш: server_map.dat
// Сервер/редактор: custom.dat в текущей рабочей папке (рядом с бинарником).
// size==0 → клиенты играют на дефолтной арене.
#define NETWORK_MAP_CACHE_PATH     "server_map.dat"
#define NETWORK_MAP_HOST_PATH      "custom.dat"
#define NETWORK_MAP_DEDICATED_PATH "custom.dat"
#define NETWORK_MAP_WAIT_PATH      "wait_map.dat"

bool Network_LoadMapFile(const char *path);
const char *Network_MapCachePath(void);
uint32_t Network_MapCrc(void);
uint32_t Network_MapSize(void);

// Клиент: последняя фаза матча из snapshot
int   Network_GetMatchPhase(void);
int   Network_GetScoreShield(void);
int   Network_GetScoreVolya(void);
float Network_GetMatchTimeLeft(void);
