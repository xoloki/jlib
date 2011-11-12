#
# Regular cron jobs for the jlib package
#
0 4	* * *	root	[ -x /usr/bin/jlib_maintenance ] && /usr/bin/jlib_maintenance
