// SPDX-License-Identifier: GPL-3.0-or-later
/* batctl execution layer -- see batman_collect.h for the contract. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "batman_collect.h"

int om_batman_collect(const char *ifname, unsigned timeout_s,
		      const char *outfile)
{
	char cmd[160];
	struct stat st;

	if (timeout_s < 1)
		timeout_s = 1;
	if (timeout_s > OM_BATCTL_TIMEOUT_MAX)
		timeout_s = OM_BATCTL_TIMEOUT_MAX;
	snprintf(cmd, sizeof(cmd),
		 "timeout %u batctl meshif %s originators -n > %s 2>/dev/null",
		 timeout_s, ifname, outfile);
	system(cmd);		/* timeout kills batctl; shell reaped here */

	if (stat(outfile, &st) < 0 || st.st_size == 0)
		return -1;
	return 0;
}
