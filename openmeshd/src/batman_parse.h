/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_BATMAN_PARSE_H
#define OPENMESH_BATMAN_PARSE_H

/*
 * Pure batctl-originators parser (no libubox, no shell): takes a FILE*
 * stream and emits om_link records through a sink callback. Kept as a
 * standalone unit so the parsing layer stays host-testable (docs/design/
 * m18-topology-model.md layering: batctl -> parser -> internal model).
 *
 * Format handled (batman-adv 2024.3, verified on device r19-r20):
 *   [B.A.T.M.A.N. adv <version>, MainIF/MAC: ...]
 *      Originator        last-seen (#/255) Nexthop           [outgoingIF]
 *    * ec:2e:98:d2:71:28    0.900s   (231) ec:2e:98:d2:71:28 [phy0-mesh0]
 *
 * Unknown header layouts (future batctl versions) return -1 instead of
 * mis-parsing; malformed data rows are counted in *skipped and dropped.
 */

#include <stdio.h>

#include "monitor.h"

typedef void (*om_link_sink)(void *priv, const struct om_link *l);

/*
 * Returns the number of originator rows parsed, or -1 when the column
 * header does not match the expected layout. *skipped (may be NULL)
 * receives the count of dropped malformed rows.
 */
int om_batman_parse(FILE *p, om_link_sink sink, void *priv,
		    unsigned *skipped);

#endif /* OPENMESH_BATMAN_PARSE_H */
