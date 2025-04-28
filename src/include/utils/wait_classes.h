/*-------------------------------------------------------------------------
 * wait_classes.h
 *	  Definitions related to wait event classes
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * src/include/utils/wait_classes.h
 * ----------
 */
#ifndef WAIT_CLASSES_H
#define WAIT_CLASSES_H


/* ----------
 * Wait Classes
 * ----------
 */
#define PG_WAIT_LWLOCK				0x01000000U
#define PG_WAIT_LOCK				0x03000000U
#define PG_WAIT_BUFFERPIN			0x04000000U
#define PG_WAIT_ACTIVITY			0x05000000U
#define PG_WAIT_CLIENT				0x06000000U
#define PG_WAIT_EXTENSION			0x07000000U
#define PG_WAIT_IPC					0x08000000U
#define PG_WAIT_TIMEOUT				0x09000000U
#define PG_WAIT_IO					0x0A000000U
#define PG_WAIT_INJECTIONPOINT		0x0B000000U

#endif							/* WAIT_CLASSES_H */
