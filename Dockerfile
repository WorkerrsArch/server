# Headless dedicated-сервер. Никакого raylib/GL/X11/ALSA - только сеть.
FROM gcc:13-bookworm AS build
WORKDIR /app
COPY network.h net_math.h network.c server_main.c ./
RUN gcc -O2 -DNETWORK_HEADLESS_BUILD -o server server_main.c network.c -lm

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/server ./server

# Внутренний порт сервера. Должен совпадать с "Internal Port",
# который вы укажете при включении TCP Proxy в настройках сервиса Railway.
ENV PORT=50000
EXPOSE 50000

CMD ["./server"]
