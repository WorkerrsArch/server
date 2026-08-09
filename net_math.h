#pragma once
// Минимальная замена Vector3 + нужных функций из raymath.h.
// Нужна только для headless-сборки сервера (NETWORK_HEADLESS_BUILD),
// чтобы не тащить в контейнер raylib и его зависимости (GL/X11/ALSA) —
// они серверу не нужны, он ничего не рисует и не проигрывает звук.
// Раскладка полей (x,y,z float) совпадает с raylib::Vector3, так что
// PlayerState/HitEvent и т.п. остаются бинарно совместимы с клиентом.

#include <math.h>

typedef struct {
    float x, y, z;
} Vector3;

static inline Vector3 Vector3Add(Vector3 a, Vector3 b) {
    return (Vector3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static inline Vector3 Vector3Subtract(Vector3 a, Vector3 b) {
    return (Vector3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline Vector3 Vector3Scale(Vector3 a, float s) {
    return (Vector3){ a.x * s, a.y * s, a.z * s };
}

static inline float Vector3DotProduct(Vector3 a, Vector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline Vector3 Vector3Lerp(Vector3 a, Vector3 b, float t) {
    return (Vector3){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

static inline float Vector3Distance(Vector3 a, Vector3 b) {
    Vector3 d = Vector3Subtract(a, b);
    return sqrtf(Vector3DotProduct(d, d));
}
