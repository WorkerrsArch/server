#pragma once
#include <stdbool.h>
#include <stdint.h>

// Headless dedicated-server: -DNETWORK_HEADLESS_BUILD, без raylib.
#ifdef NETWORK_HEADLESS_BUILD
    #include "net_math.h"
#else
    #include "raylib.h"
#endif

/* Должно совпадать с клиентом (sizeof GameSnapshot на проводе). */
#define MAX_PLAYERS 8              // 4v4
#define SERVER_PORT 50000
#define BUFFER_SIZE 4096
#define CLIENT_TIMEOUT 5.0f
#define MAX_PACKETS_PER_TICK 64
#define HITSCAN_RANGE 100.0f

/* ---- TDM: Щит vs Воля (как у клиента) ---- */
#define MATCH_SCORE_WIN      150
#define MATCH_ROUND_SEC      600.0f
#define MATCH_WAIT_SEC       20.0f
#define MATCH_END_HOLD_SEC   15.0f
#define MATCH_PTS_KILL       15
#define MATCH_PTS_DAMAGE_DIV 5

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

extern bool isServer;

bool Network_InitServer(void);
bool Network_InitClient(const char *serverAddr);
void Network_SendState(PlayerState state);
bool Network_ReceiveSnapshot(GameSnapshot *snap);
void Network_BroadcastSnapshot(GameSnapshot snap);
void Network_SendHitEvent(HitEvent event);
bool Network_ReceiveHitEvent(HitEvent *event);
void Network_BroadcastHitConfirm(HitConfirm confirm);
bool Network_ReceiveHitConfirm(HitConfirm *confirm);
void Network_Close(void);
int  Network_GetMyId(void);
float Network_GetPing(void);
void Network_SeedPing(float ms);
bool Network_ProbeServer(const char *addr, int timeoutMs, float *outPingMs);

void Network_HostFire(HitEvent event);
PlayerState Network_GetSelfAuthoritativeState(void);

bool Network_InitDedicatedServer(int port);

#define NETWORK_MAP_CACHE_PATH     "server_map.dat"
#define NETWORK_MAP_HOST_PATH      "custom.dat"
#define NETWORK_MAP_DEDICATED_PATH "custom.dat"

bool Network_LoadMapFile(const char *path);
const char *Network_MapCachePath(void);
uint32_t Network_MapCrc(void);
uint32_t Network_MapSize(void);

int   Network_GetMatchPhase(void);
int   Network_GetScoreShield(void);
int   Network_GetScoreVolya(void);
float Network_GetMatchTimeLeft(void);
