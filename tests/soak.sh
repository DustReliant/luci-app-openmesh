#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# V0.2 soak sampler: run ON the controller device (cron or nohup).
# Samples daemon health, session state and memory into a CSV for the
# 72h stability gate (roadmap V0.2 -> V0.3).
#
#   ssh root@<device> 'nohup sh /tmp/soak.sh >/dev/null 2>&1 &'
#   # stop with: touch /tmp/soak.stop

OUT=/root/soak.csv
INT=60

echo "ts,uptime_s,nodes_total,adopted,online,rss_kb" > "$OUT"
while [ ! -f /tmp/soak.stop ]; do
	TS=$(date +%s)
	ST=$(ubus call openmesh status 2>/dev/null || echo '{}')
	AD=$(ubus call openmesh list_adopted 2>/dev/null || echo '{"nodes":[]}')
	UP=$(echo "$ST" | jsonfilter -e '@.uptime' 2>/dev/null || echo 0)
	NT=$(echo "$ST" | jsonfilter -e '@.nodes_total' 2>/dev/null || echo 0)
	NA=$(echo "$AD" | jsonfilter -e '@.nodes[*].id' 2>/dev/null | wc -l)
	ON=$(echo "$AD" | jsonfilter -e '@.nodes[*].online' 2>/dev/null | \
		grep -c true)
	RSS=$(awk '/VmRSS/{print $2}' /proc/$(pidof openmeshd)/status 2>/dev/null)
	echo "$TS,$UP,$NT,$NA,$ON,$RSS" >> "$OUT"
	[ -f /tmp/soak.stop ] || sleep "$INT"
done
rm -f /tmp/soak.stop
