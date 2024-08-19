/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.h
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "fe_utils/psqlscan.h"

typedef enum {
    Router,
    Shard,
} NodeType;

extern char* SendQueryToShard(char* query_data);
extern void init_node_instance(NodeType nodeType, char* port, char* config_file_path);

extern const PsqlScanCallbacks psqlscan_callbacks;

extern int	MainLoop(FILE *source);

extern char* SendQueryToShard(char* query_data);

#endif							/* MAINLOOP_H */
