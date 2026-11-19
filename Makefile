CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=
LDLIBS  ?= -pthread

HDR := $(wildcard include/*.h)

COMMON := src/config.c src/io.c src/model.c
APP    := src/app.c
NET    := src/net.c

SERVER_SRC := $(COMMON) $(APP) src/controller.c src/direction.c src/server.c $(NET) apps/app_server.c
CLIENT_SRC := $(COMMON) $(APP) src/client.c $(NET)

BINS := app_server app_client_car app_client_ped app_observer

all: $(BINS)

app_server: $(SERVER_SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SERVER_SRC) $(LDLIBS)

app_client_car: $(CLIENT_SRC) apps/app_client_car.c $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLIENT_SRC) apps/app_client_car.c $(LDLIBS)

app_client_ped: $(CLIENT_SRC) apps/app_client_ped.c $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLIENT_SRC) apps/app_client_ped.c $(LDLIBS)

app_observer: $(CLIENT_SRC) apps/app_observer.c $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLIENT_SRC) apps/app_observer.c $(LDLIBS)

run: all
	./run_server.sh

test: all
	./tests/run_tests.sh

clean:
	rm -f $(BINS) *.log tests/out_*.log

.PHONY: all run test clean
