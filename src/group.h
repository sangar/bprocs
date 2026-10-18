#pragma once

#include "proc.h"

// The list shows top-level procs as they are, and the procs of each app of
// include:, named app:proc, under a header for the app: a group.

// Group is where the procs of an app are; they follow one another.
typedef struct Group {
    String name;
    size_t first;
    size_t count;
    bool expanded;
} Group;

// Row is a line of the list: a group's header, or a proc.
typedef struct Row {
    bool header;
    bool grouped; // a header, or a proc under one
    size_t group; // when grouped
    size_t proc;  // a proc's index; for a header, its group's first proc
} Row;

// group_find finds the groups among names and writes them, collapsed, to
// groups, which has room for one per name; it returns how many.
size_t group_find(StringList names, Group *groups);
// group_rows writes the rows the list shows to rows, which has room for one
// per proc and group, and returns how many: each header, then its procs when
// the group is expanded.
size_t group_rows(const Group *groups, size_t group_count, size_t proc_count, Row *rows);
// group_short_name is name without its app: prefix.
String group_short_name(String name);
// GroupRank orders statuses by how much they need attention: a header shows
// the status of its proc ranked highest.
typedef enum GroupRank {
    RANK_DONE, // a job that succeeded
    RANK_IDLE,
    RANK_STOPPED,
    RANK_RUNNING,
    RANK_CHANGING, // starting, stopping or waiting
    RANK_RESTARTING,
    RANK_UNHEALTHY,
    RANK_FAILED, // crashed, failed or blocked, with no restart coming
} GroupRank;

GroupRank group_rank(ProcStatus status);
