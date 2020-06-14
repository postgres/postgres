src/backend/replication/README

Walreceiver - libpqwalreceiver API
----------------------------------

The transport-specific part of walreceiver, responsible for connecting to
the primary server, receiving WAL files and sending messages, is loaded
dynamically to avoid having to link the main server binary with libpq.
The dynamically loaded module is in libpqwalreceiver subdirectory.

The dynamically loaded module implements a set of functions with details
about each one of them provided in src/include/replication/walreceiver.h.

This API should be considered internal at the moment, but we could open it
up for 3rd party replacements of libpqwalreceiver in the future, allowing
pluggable methods for receiving WAL.

Walreceiver IPC
---------------

When the WAL replay in startup process has reached the end of archived WAL,
restorable using restore_command, it starts up the walreceiver process
to fetch more WAL (if streaming replication is configured).

Walreceiver is a postmaster subprocess, so the startup process can't fork it
directly. Instead, it sends a signal to postmaster, asking postmaster to launch
it. Before that, however, startup process fills in WalRcvData->conninfo
and WalRcvData->slotname, and initializes the starting point in
WalRcvData->receiveStart.

As walreceiver receives WAL from the primary server, and writes and flushes
it to disk (in pg_wal), it updates WalRcvData->flushedUpto and signals
the startup process to know how far WAL replay can advance.

Walreceiver sends information about replication progress to the primary server
whenever it either writes or flushes new WAL, or the specified interval elapses.
This is used for reporting purpose.

Walsender IPC
-------------

At shutdown, postmaster handles walsender processes differently from regular
backends. It waits for regular backends to die before writing the
shutdown checkpoint and terminating pgarch and other auxiliary processes, but
that's not desirable for walsenders, because we want the standby servers to
receive all the WAL, including the shutdown checkpoint, before the primary
is shut down. Therefore postmaster treats walsenders like the pgarch process,
and instructs them to terminate at PM_SHUTDOWN_2 phase, after all regular
backends have died and checkpointer has issued the shutdown checkpoint.

When postmaster accepts a connection, it immediately forks a new process
to handle the handshake and authentication, and the process initializes to
become a backend. Postmaster doesn't know if the process becomes a regular
backend or a walsender process at that time - that's indicated in the
connection handshake - so we need some extra signaling to let postmaster
identify walsender processes.

When walsender process starts up, it marks itself as a walsender process in
the PMSignal array. That way postmaster can tell it apart from regular
backends.

Note that no big harm is done if postmaster thinks that a walsender is a
regular backend; it will just terminate the walsender earlier in the shutdown
phase. A walsender will look like a regular backend until it's done with the
initialization and has marked itself in PMSignal array, and at process
termination, after unmarking the PMSignal slot.

Each walsender allocates an entry from the WalSndCtl array, and tracks
information about replication progress. User can monitor them via
statistics views.


Walsender - walreceiver protocol
--------------------------------

See manual.
