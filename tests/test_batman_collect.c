// SPDX-License-Identifier: GPL-3.0-or-later
/* Tiny harness around om_batman_collect for the command-layer shell
 * tests: argv = <ifname> <timeout_s> <outfile>; exit 0 on success. */
#include <stdio.h>
#include <stdlib.h>

#include "batman_collect.h"

int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	return om_batman_collect(argv[1], (unsigned)atoi(argv[2]),
				 argv[3]) == 0 ? 0 : 1;
}
