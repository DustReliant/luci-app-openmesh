/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_STORE_H
#define OPENMESH_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Persisted adopted-node record (UCI "config node" sections). */
struct om_stored_node {
	uint8_t id[32];
	uint8_t pk[32];
	char name[24];
};

/* Append an adopted node; returns 0 / -1. */
int om_store_adopted(const uint8_t id[32], const uint8_t pk[32],
		     const char *name);

/* Rename an adopted node (matched by id). Returns 0 / -1. */
int om_store_rename(const uint8_t id[32], const char *name);

/* Remove an adopted node (matched by id). Returns 0 / -1. */
int om_store_remove(const uint8_t id[32]);

/*
 * Enumerate stored nodes; cb returns 0 to continue, non-zero to stop.
 * Bad records are skipped with a log line. Returns count of valid records.
 */
int om_store_foreach(int (*cb)(const struct om_stored_node *n, void *priv),
		     void *priv);

#endif /* OPENMESH_STORE_H */
