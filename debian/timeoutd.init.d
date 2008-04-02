#! /bin/sh
### BEGIN INIT INFO
# Provides:          timeoutd
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: start and stop the user timeout daemon
### END INIT INFO
#
# Based on skeleton 1.9.1 by Miquel van Smoorenburg <miquels@cistron.nl>.

DAEMON=/usr/sbin/timeoutd
NAME=timeoutd
DESC="user timeout daemon"

test -x $DAEMON || exit 0

set -e

case "$1" in
  start)
	echo -n "Starting $DESC: $NAME"
	start-stop-daemon --start --oknodo --quiet --exec $DAEMON
	echo "."
	;;
  stop)
	echo -n "Stopping $DESC: $NAME"
	start-stop-daemon --stop --oknodo --quiet --exec $DAEMON
	echo "."
	;;
  reload|force-reload)
	echo -n "Reloading $DESC configuration..."
	start-stop-daemon --stop --signal 1 --quiet --exec $DAEMON
	echo "done."
  	;;
  restart)
	echo -n "Restarting $DESC: $NAME"
	start-stop-daemon --stop --oknodo --quiet --exec $DAEMON
	sleep 1
	start-stop-daemon --start --quiet --exec $DAEMON
	echo "."
	;;
  *)
	N=/etc/init.d/$NAME
	echo "Usage: $N {start|stop|restart|reload|force-reload}" >&2
	exit 1
	;;
esac

exit 0
