/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_BATMAN_COLLECT_H
#define OPENMESH_BATMAN_COLLECT_H

/*
 * batctl execution layer, fully separated from parsing (batman_parse.c):
 * this unit only knows how to RUN batctl with a bounded lifetime and
 * stage its raw output; it never interprets a byte of it.
 *
 * Timeout policy (r21b acceptance):
 *  - configurable per call (UCI openmesh.main.batctl_timeout, default
 *    OM_BATCTL_TIMEOUT_DEFAULT, clamped 1..30s);
 *  - `timeout` kills a wedged batctl; system() reaps the shell, so no
 *    zombies are left behind;
 *  - worst-case main-loop stall is the timeout itself (batctl normally
 *    answers in <50ms); a fully async child would restructure the
 *    verified monitor loop and is deliberately not done (see
 *    docs/design/m18-topology-model.md r21b);
 *  - a failed/timed-out collection is reported as a return code, NEVER
 *    as "link gone": callers must retain last-known state.
 */

#define OM_BATCTL_TIMEOUT_DEFAULT 2
#define OM_BATCTL_TIMEOUT_MAX     30

/*
 * Runs `timeout <t> batctl meshif <ifname> originators -n` into outfile.
 * Returns 0 when the file exists and is non-empty, -1 otherwise
 * (command missing, timeout, empty mesh -- indistinguishable by
 * design: the caller treats all as COLLECT_FAILED).
 */
int om_batman_collect(const char *ifname, unsigned timeout_s,
		      const char *outfile);

#endif /* OPENMESH_BATMAN_COLLECT_H */
