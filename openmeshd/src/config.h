/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_CONFIG_H
#define OPENMESH_CONFIG_H

#include <stdbool.h>

struct om_main_cfg {
	bool enabled;
	char role[16];
	char model[48];
	char fw[32];
	int interval;		/* announce interval, seconds */
	int port;		/* announce/listen port */
};

/*
 * Load /etc/config/openmesh (main section) plus system identity info
 * (/tmp/sysinfo/model, /etc/openwrt_release). Missing config falls back
 * to defaults; returns -1 only on context allocation failure.
 */
int om_main_load(struct om_main_cfg *m);

#endif /* OPENMESH_CONFIG_H */
