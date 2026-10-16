#pragma once

#include "config.h"
#include "mc/encoding/node.h"
#include "proc.h"
#include "stats.h"

// The daemon and its clients talk in frames (mc/platform/frame.h) whose headers are JSON
// objects with a "type". A request carries an "id", and its reply, of type
// "reply", the same id and an "error" when it failed. The daemon also sends
// events to subscribed clients: output, note, state, stats, cleared and bye.

enum {
    PROTOCOL_VERSION = 1,
    PROTOCOL_FRAME_LIMIT = 17 << 20, // a header and a log of the whole scrollback fit
};

// RuntimePaths are where the daemon of one config keeps its files, in the
// user's private runtime directory, named after the config's path.
typedef struct RuntimePaths {
    String directory;
    String id;     // the first 12 hex digits of the SHA-256 of the config path
    String socket; // where the daemon, or bprocs run, listens
    String lock;   // held by whoever listens, for as long as it does
    String start;  // held by a client while it starts the daemon
    String log;    // the daemon's own output
} RuntimePaths;

// runtime_paths makes the runtime directory when it is missing.
[[nodiscard]] Error runtime_paths(Arena *arena, String config_path, RuntimePaths *paths, Err *err);
// runtime_directory is the directory every daemon of this user keeps its files in.
[[nodiscard]] Error runtime_directory(Arena *arena, String *directory, Err *err);

// message starts a header of the given type.
Node *message(Arena *arena, const char *type);
void message_int(Arena *arena, Node *message, const char *key, int64_t value);
void message_text(Arena *arena, Node *message, const char *key, String value);
void message_bool(Arena *arena, Node *message, const char *key, bool value);
void message_put(Arena *arena, Node *message, const char *key, Node *value);
Node *message_list(Arena *arena);
void message_append_text(Arena *arena, Node *list, String value);
void message_append_int(Arena *arena, Node *list, int64_t value);
String message_encode(Arena *arena, const Node *message);
// message_parse reads a header, which must be an object with a "type".
[[nodiscard]] Error message_parse(Arena *arena, String header, Node **message, Err *err);
String message_type(const Node *message);
// message_error is the error of a reply, empty when it succeeded.
String message_error(const Node *message);

// A spec crosses the socket with what clients show of it. A ProcSpec read
// back has no argv or env, and its checks hold only their kind and, in
// text, what they look at.
Node *spec_to_node(Arena *arena, const ProcSpec *spec, const Config *config);
// specs_from_node reads a list of specs, each with its needs as indexes into the list.
ProcSpec *specs_from_node(Arena *arena, const Node *list, size_t *count);

Node *status_to_node(Arena *arena, ProcStatus status);
ProcStatus status_from_node(const Node *node);

// history_to_node lists a history's samples from oldest to newest, and
// history_from_node pushes such a list into a history.
Node *history_to_node(Arena *arena, const History *history);
void history_from_node(const Node *node, History *history);
