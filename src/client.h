#pragma once

#include "config.h"
#include "mc/encoding/node.h"

// Client is a connection to the daemon of one config, after hello.
typedef struct Client Client;

// ClientInfo is what the daemon says about itself in its hello.
typedef struct ClientInfo {
    String config_path;
    String theme;
    size_t scrollback;
    int pid;
    ProcSpec *specs; // what clients show of each proc; see specs_from_node
    size_t count;
} ClientInfo;

// client_ensure_daemon connects to the daemon of the config at config_path,
// starting it first when none runs; *started says whether it did.
[[nodiscard]] Error client_ensure_daemon(Arena *scratch, String config_path, bool *started, Err *err);

// client_connect connects to the daemon of the config at config_path and says
// hello. No daemon is ERR_NOT_FOUND; one from another version is ERR_UNSUPPORTED.
[[nodiscard]] Error client_connect(String config_path, Client **client, Err *err);
// client_connect_socket connects to the daemon listening on socket.
[[nodiscard]] Error client_connect_socket(String socket, Client **client, Err *err);
void client_close(Client *client);
const ClientInfo *client_info(const Client *client);
bool client_proc_index(const Client *client, String name, size_t *index);

// client_request sends a request and waits for its reply, which it reads
// into arena with its body, passing over events. A reply with an error is
// ERR_IO with the error in err. It must not be used after client_listen.
[[nodiscard]] Error client_request(Client *client, Arena *arena, Node *request, String body, Node **reply,
                                   String *reply_body, Err *err);
// client_wait_closed reads and drops what the daemon sends until it closes
// the connection, as it does once it has gone down.
void client_wait_closed(Client *client);
// client_send sends a request without waiting for its reply.
[[nodiscard]] Error client_send(Client *client, Node *request, String body, Err *err);

// ClientMessage is a frame the daemon sent, in an arena of its own; header is
// nullptr when the connection has closed.
typedef struct ClientMessage {
    Arena *arena;
    Node *header;
    String body;
} ClientMessage;

// client_listen starts a thread that reads everything the daemon sends and
// queues it, calling wake(context) after each frame.
[[nodiscard]] Error client_listen(Client *client, void (*wake)(void *context), void *context, Err *err);
// client_next takes the next frame read, without waiting; false when none waits.
bool client_next(Client *client, ClientMessage *message);
void client_message_release(ClientMessage *message);
