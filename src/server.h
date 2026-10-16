#pragma once

#include "config.h"
#include "protocol.h"
#include "supervisor.h"

// Server runs a config's supervisor and serves it on a Unix socket: clients
// ask it to start and stop procs, read their logs, wait for them, and
// subscribe to their output, states and stats. The daemon and bprocs run are
// both a Server and a loop around server_step. Everything but the two
// server_request_ functions runs on the thread that created it.
typedef struct Server Server;

// server_create takes the config's lock, which it holds until destroyed, and
// listens on its socket with permissions 0600, replacing a stale one. A lock
// that another daemon or bprocs run holds is ERR_IO. Nothing starts until a
// client asks.
[[nodiscard]] Error server_create(const Config *config, const RuntimePaths *paths, Server **server, Err *err);
// server_destroy tells every client it is going, closes the socket, and
// kills any proc that still runs.
void server_destroy(Server *server);

Supervisor *server_supervisor(Server *server);

// server_step waits up to wait_ms for something to do, and does it: client
// requests, the supervisor's tick, stats once a second, events to clients.
void server_step(Server *server, int wait_ms);
// server_finished is whether a down has finished, so the loop can end.
bool server_finished(Server *server);

// server_allow_reload lets clients ask for the config to be read again. A
// server whose owner keeps its own view of the procs, as bprocs run does,
// leaves it off.
void server_allow_reload(Server *server);

// server_request_reload asks for the config to be read again, as a client
// would, from any thread; it does nothing unless reload is allowed.
void server_request_reload(Server *server);

// server_request_down asks for a down as a client would, from any thread,
// such as one that waits for signals.
void server_request_down(Server *server);
