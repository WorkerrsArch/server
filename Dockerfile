# Headless dedicated-сервер. Никакого raylib/GL/X11/ALSA - только сеть.
FROM gcc:13-bookworm AS build
WORKDIR /app
COPY network.h net_math.h network.c server_main.c ./
RUN gcc -O2 -DNETWORK_HEADLESS_BUILD -o server server_main.c network.c -lm

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/server ./server
# Карта: custom.dat рядом с server (из редактора: S → custom.dat)
COPY custom.dat ./custom.dat
# Опционально: spawn_points.cfg рядом с custom.dat

ENV PORT=50000
# 60 Гц по умолчанию; можно TICK_HZ=30 на очень слабом железе
ENV TICK_HZ=60
EXPOSE 50000

CMD ["./server"]
