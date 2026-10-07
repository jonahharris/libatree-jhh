/* The command hook the vendored pogocache connection layer calls once a
 * request has been parsed into arguments (see deps/pogocache/conn.c). */
#ifndef CMDS_H
#define CMDS_H

#include "args.h"
#include "conn.h"

void evcommand(struct conn *conn, struct args *args);
/* Called when a connection closes, before its state is freed. */
void evclosed_hook(struct conn *conn);

#endif
