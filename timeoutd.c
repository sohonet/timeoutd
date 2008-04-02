/*
		"@(#) timeoutd.c 1.6 by Shane Alderton"
			based on:
		"@(#) autologout.c by David Dickson" 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Thanks to:
    	David Dickson for writing the original autologout.c
    	programme upon which this programme was based.

*/
/* #define DEBUG _DEBUG_ */
#include    <unistd.h>
#include    <stdlib.h>
#include    <sys/ioctl.h>
#include    <fcntl.h>
#include    <stdio.h>
#include    <signal.h>
#include    <string.h>
#include    <sys/types.h>
#include    <sys/wait.h>
#include    <sys/stat.h>
#include    <utmp.h>
#include    <pwd.h>
#include    <grp.h>
#include    <sys/syslog.h>
#include    <time.h>
#include    <ctype.h>
#include    <termios.h>
#include    <fcntl.h>
#include    <dirent.h>

#ifdef TIMEOUTDX11
#include <netdb.h>
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>

#define TIMEOUTD_XSESSION_NONE 0
#define TIMEOUTD_XSESSION_LOCAL 1
#define TIMEOUTD_XSESSION_REMOTE 2
#endif

#define OPENLOG_FLAGS	LOG_CONS|LOG_PID
#define SYSLOG_DEBUG	LOG_DEBUG

/* For those systems (SUNOS) which don't define this: */
#ifndef WTMP_FILE
#define WTMP_FILE "/usr/adm/wtmp"
#endif

#ifdef SUNOS
#define ut_pid ut_time
#define ut_user ut_name
#define SEEK_CUR 1
#define SEEK_END 2
#define SURE_KILL 1

FILE	*utfile = NULL;
#define NEED_UTMP_UTILS
#define NEED_STRSEP
#endif


#ifdef NEED_UTMP_UTILS
void setutent()
{
	if (utfile == NULL)
	{
		if ((utfile = fopen("/etc/utmp", "r")) == NULL)
		{
			openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
			syslog(LOG_ERR, "Could not open /etc/utmp");
			closelog();
			exit(1);
		}
	}
	else fseek(utfile, 0L, 0);
}

struct utmp *getutent()    /* returns next utmp file entry */
{
	static struct utmp	uent;

	while (fread(&uent, sizeof(struct utmp), 1, utfile) == 1)
	{
		if (uent.ut_line[0] != 0 && uent.ut_name[0] != 0) 
			return &uent;
	}
	return (struct utmp *) NULL;
}
#endif

#ifndef linux
#define N_TTY 1
#define N_SLIP 2
#endif

#ifndef CONFIG
#define CONFIG "/etc/timeouts"
#endif

#define MAXLINES 512
#define max(a,b) ((a)>(b)?(a):(b))

#define ACTIVE		1
#define IDLEMAX		2
#define SESSMAX		3
#define DAYMAX		4
#define NOLOGIN		5
/*#define XSESSION	6*/
#define	IDLEMSG		0
#define	SESSMSG		1
#define	DAYMSG		2
#define	NOLOGINMSG	3

#define KWAIT		5  /* Time to wait after sending a kill signal */

char	*limit_names[] = {"idle", "session", "daily", "nologin"};

char	*daynames[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA", "WK", "AL", NULL};
char	daynums[] = {   1  ,  2  ,  4  ,  8  ,  16 ,  32 ,  64 ,  62 ,  127, 0};

struct utmp *utmpp;         /* pointer to utmp file entry */
char        *ctime();       /* returns pointer to time string */
struct utmp *getutent();    /* returns next utmp file entry */
void	    shut_down();
void	    read_config();
void	    reread_config();
void        reapchild();
void        free_wtmp();
void        check_idle();
void        read_wtmp();
void        bailout();
char	    chk_timeout();
void	    logoff_msg();
void	    killit();
int	    getdisc();
int	    chk_xsession(); /* seppy: is it a X-Session? */
void	    killit_xsession(); /* seppy: kill the X-Session*/
int 	    chk_ssh(pid_t pid); /* seppy: check if user is logged in via ssh (we have to
handle that different... ;( */
char	    *getusr(pid_t pid); /*seppy: get the owner of a running process */
void	    segfault(); /* seppy: catch segfault and log them */
int	    chk_xterm(); /* seppy: is it a xterm? */
pid_t	    getcpid(); /* seppy: get the child's pid. Needed for ssh */

#ifdef TIMEOUTDX11
Time	    get_xidle(); /* seppy: how long is user idle? (user,display)*/
#endif


struct ut_list {
	struct utmp	elem;
	struct ut_list	*next;
};

struct ut_list	*wtmplist = (struct ut_list *) NULL;
struct ut_list	*ut_list_p;

struct time_ent {
	int	days;
	int	starttime;
	int	endtime;
};

struct config_ent {
	struct time_ent	*times;
	char	*ttys;
	char	*users;
	char	*groups;
	char	login_allowed;
	int	idlemax;
	int	sessmax;
	int	daymax;
	int	warntime;
	char	*messages[10];
};

struct config_ent	*config[MAXLINES + 1];
char	errmsg[256];
char	dev[sizeof(utmpp->ut_line)];
unsigned char	limit_type;
int	configline = 0;
int	pending_reread = 0;
int	allow_reread = 0;
time_t	time_now;
struct tm	now;
int		now_hhmm;
int	daytime = 0;	/* Amount of time a user has been on in current day */
char path[255]; /*seppy*/
FILE *proc_file;/*seppy*/
char comm[16]; /*seppy; to save the command of a pid*/

#ifdef NEED_STRCASECMP
int strcasecmp(char *s1, char *s2)
{
	while (*s1 && *s2)
	{
	  if (tolower(*s1) < tolower(*s2))
	  	return -1;
	  else if (tolower(*s1) > tolower(*s2))
	  	return 1;
	  s1++;
	  s2++;
	}
	if (*s1)
		return -1;
	if (*s2)
		return 1;
	return 0;
}
#endif

#ifdef NEED_STRSEP
char *strsep (stringp, delim)
char **stringp;
char *delim;
{
	char	*retp = *stringp;
	char	*p;

	if (!**stringp) return NULL;

	while (**stringp)
	{
		p = delim;
		while (*p)
		{
		  if (*p == **stringp)
		  {
		    **stringp = '\0';
		    (*stringp)++;
		    return retp;
		  }
		  p++;
	  	}
	  	(*stringp)++;
	}
	return retp;
}
#endif

int main(argc, argv)
int	argc;
char	*argv[];
{
    signal(SIGTERM, shut_down);
    signal(SIGHUP, reread_config);
    signal(SIGCHLD, reapchild);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGSEGV, segfault);

    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);

/* The only valid invocations are "timeoutd" or "timeoutd user tty" */
    if (argc != 1 && argc != 3)
    {
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_ERR, "Incorrect invocation of timeoutd (argc=%d) by UID %d.", argc, getuid());
      closelog();
      exit(5);
    }

    /* read config file into memory */
    read_config();

/* Change into the root filesystem to avoid "device busy" errors when the
 * filesystem from which we were started is unmounted.  /dev is convenient as
 * ut_line fields are relative to it.
 */
    if (chdir("/dev"))
    {
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_ERR, "Could not change working directory to /dev!");
      closelog();
      exit(1);
    }

/* Handle the "timeoutd user tty" invocation */
/* This is a bit of a shameless hack, but, well, it works. */
    if (argc == 3)
    {
#ifdef DEBUG
        openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(SYSLOG_DEBUG, "Running in user check mode.  Checking user %s on %s.", argv[1], argv[2]);
        closelog();
#endif
    	strncpy(dev, argv[2], sizeof(dev) - 1);
    	dev[sizeof(dev) - 1] = '\0';
        time_now = time((time_t *)0);  /* get current time */
        now = *(localtime(&time_now));  /* Break it into bits */
        now_hhmm = now.tm_hour * 100 + now.tm_min;
        allow_reread = 0;
        read_wtmp(); /* Read in today's wtmp entries */
        switch(chk_timeout(argv[1], dev, "",  0, 0))
        {
            case DAYMAX:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_NOTICE,
		       "User %s on %s exceeded maximum daily limit (%d minutes).  Login check failed.",
		       argv[1], argv[2], config[configline]->daymax);
    		closelog();
    		/*
    		printf("\r\nLogin not permitted.  You have exceeded your maximum daily limit.\r\n");
    		printf("Please try again tomorrow.\r\n");
    		*/
    		logoff_msg(1);
    		exit(10);
            case NOLOGIN:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_NOTICE,
		       "User %s not allowed to login on %s at this time.  Login check failed.",
		       argv[1], argv[2]);
    		closelog();
    		/*
    		printf("\r\nLogin not permitted at this time.  Please try again later.\r\n");
    		*/
    		logoff_msg(1);
    		exit(20);
            case ACTIVE:
#ifdef DEBUG
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(SYSLOG_DEBUG, "User %s on %s passed login check.", argv[1], argv[2]);
    		closelog();
#endif
		free_wtmp();
		exit(0);
            default:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_ERR, "Internal error checking user %s on %s - unexpected return from chk_timeout",
			argv[1], argv[2]);
    		closelog();
    		exit(30);
        }
    }

    /* If running in daemon mode (no parameters) */
    if (fork())             /* the parent process */
        exit(0);            /* exits */

    close(0);
    close(1);
    close(2);
    setsid();

    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(LOG_NOTICE, "Daemon started.");
    closelog();

    /* the child processes all utmp file entries: */
    while (1)
    {
        /* Record time at which we woke up & started checking */
        time_now = time((time_t *)0);  /* get current time */
        now = *(localtime(&time_now));  /* Break it into bits */
        now_hhmm = now.tm_hour * 100 + now.tm_min;
        allow_reread = 0;
        read_wtmp(); /* Read in today's wtmp entries */
    	setutent();
#ifdef DEBUG
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    	syslog(SYSLOG_DEBUG, "Time to check utmp for exceeded limits.");
    	closelog();
#endif
        while ((utmpp = getutent()) != (struct utmp *) NULL)
            check_idle();
        free_wtmp();  /* Free up memory used by today's wtmp entries */
        allow_reread = 1;
        if (pending_reread)
           reread_config(SIGHUP);
#ifdef DEBUG
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(SYSLOG_DEBUG, "Finished checking utmp... sleeping for 1 minute.");
	closelog();
#endif
        sleep(60);
    }
}

/* Read in today's wtmp entries */

void read_wtmp()
{
    FILE	*fp;
    struct utmp	ut;
    struct tm	*tm;

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Reading today's wtmp entries.");
    closelog();
#endif

    if ((fp = fopen(WTMP_FILE, "r")) == NULL)
      bailout("Could not open wtmp file!", 1);

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Seek to end of wtmp");
    closelog();
#endif
    /* Go to end of file minus one structure */
    fseek(fp, -1L * sizeof(struct utmp), SEEK_END);

    while (fread(&ut, sizeof(struct utmp), 1, fp) == 1)
    {
      tm = localtime(&ut.ut_time);

      if (tm->tm_year != now.tm_year || tm->tm_yday != now.tm_yday)
        break;

#ifndef SUNOS
      if (ut.ut_type == USER_PROCESS ||
          ut.ut_type == DEAD_PROCESS ||
          ut.ut_type == UT_UNKNOWN ||	/* SA 19940703 */
	  ut.ut_type == LOGIN_PROCESS ||
          ut.ut_type == BOOT_TIME)
#endif
      {
        if ((ut_list_p = (struct ut_list *) malloc(sizeof(struct ut_list))) == NULL)
          bailout("Out of memory in read_wtmp.", 1);
        ut_list_p->elem = ut;
        ut_list_p->next = wtmplist;
        wtmplist = ut_list_p;
      }

      /* Position the file pointer 2 structures back */
      if (fseek(fp, -2 * sizeof(struct utmp), SEEK_CUR) < 0) break;
    }
    fclose(fp);
#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Finished reading today's wtmp entries.");
    closelog();
#endif
}

/* Free up memory used by today's wtmp entries */

void free_wtmp()
{
#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Freeing list of today's wtmp entries.");
    closelog();
#endif

    while (wtmplist)
    {
#ifdef DEBUG_WTMP
    struct tm	*tm;
    tm = localtime(&(wtmplist->elem.ut_time));
    printf("%d:%d %s %s %s\n", 
    	tm->tm_hour,tm->tm_min, wtmplist->elem.ut_line,
    	wtmplist->elem.ut_user,
#ifndef SUNOS
    	wtmplist->elem.ut_type == LOGIN_PROCESS?"login":wtmplist->elem.ut_type == BOOT_TIME?"reboot":"logoff"
#else
	""
#endif
	);
#endif
        ut_list_p = wtmplist;
        wtmplist = wtmplist->next;
        free(ut_list_p);
    }
#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Finished freeing list of today's wtmp entries.");
    closelog();
#endif
}

void	store_times(t, time_str)
struct time_ent **t;
char *time_str;
{
    int	i = 0;
    int	ar_size = 2;
    char	*p;
    struct time_ent	*te;

    while (time_str[i])
      if (time_str[i++] == ',')
        ar_size++;

    if ((*t = (struct time_ent *) malloc (ar_size * sizeof(struct time_ent))) == NULL)
	bailout("Out of memory", 1);
    te = *t;

    p = strtok(time_str, ",");
/* For each day/timerange set, */
    while (p)
    {
/* Store valid days */
      te->days = 0;
      while (isalpha(*p))
      {
        if (!p[1] || !isalpha(p[1]))
        {
	  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
          syslog(LOG_ERR, "Malformed day name (%c%c) in time field of config file (%s).  Entry ignored.", p[0], p[1], CONFIG);
          closelog();
          (*t)->days = 0;
          return;
        }
        *p = toupper(*p);
        p[1] = toupper(p[1]);

      	i = 0;
      	while (daynames[i])
      	{
      	  if (!strncmp(daynames[i], p, 2))
      	  {
      	    te->days |= daynums[i];
      	    break;
      	  }
      	  i++;
      	}
      	if (!daynames[i])
      	{
	  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
          syslog(LOG_ERR, "Malformed day name (%c%c) in time field of config file (%s).  Entry ignored.", p[0], p[1], CONFIG);
          closelog();
          (*t)->days = 0;
          return;
      	}
      	p += 2;
      }

/* Store start and end times */
      if (*p)
      {
        if (strlen(p) != 9 || p[4] != '-')
        {
	  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
          syslog(LOG_ERR, "Malformed time (%s) in time field of config file (%s).  Entry ignored.", p, CONFIG);
          closelog();
          (*t)->days = 0;
          return;
        }
        te->starttime = atoi(p);
        te->endtime = atoi(p+5);
        if ((te->starttime == 0 && strncmp(p, "0000-", 5)) || (te->endtime == 0 && strcmp(p+5, "0000")))
        {
	  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
          syslog(LOG_ERR, "Invalid range (%s) in time field of config file (%s).  Entry ignored.", p, CONFIG);
          closelog();
          (*t)->days = 0;
          return;
        }
      }
      else
      {
      	te->starttime = 0;
      	te->endtime = 2359;
      }
      p = strtok(NULL, ",");
      te++;
    }
    te->days = 0;
}

void alloc_cp(a, b)
char **a;
char *b;
{
	if ((*a = (char *) malloc(strlen(b)+1)) == NULL)
		bailout("Out of memory", 1);
	else	strcpy(*a, b);
}

void read_config()
{
    FILE	*config_file;
    char	*p;
    char	*lstart;
    int		i = 0;
#ifdef DEBUG
    int		j = 0;
#endif
    char	line[256];
    char	*tok;
    int		linenum = 0;

    if ((config_file = fopen(CONFIG, "r")) == NULL)
      bailout("Cannot open config file", 1);

    while (fgets(line, 256, config_file) != NULL)
    {
      linenum++;
      p = line;
      while  (*p && (*p == ' ' || *p == '\t'))
        p++;
      lstart = p;
      while (*p && *p != '#' && *p != '\n')
        p++;
      *p = '\0';
      if (*lstart)
      {
      	if (i == MAXLINES)
      	  bailout("Too many lines in timeouts config file.", 1);
        if ((config[i] = (struct config_ent *) malloc(sizeof(struct config_ent))) == NULL)
          bailout("Out of memory", 1);
  	config[i]->times = NULL;
  	config[i]->ttys = NULL;
  	config[i]->users = NULL;
  	config[i]->groups = NULL;
  	config[i]->login_allowed = 1;
  	config[i]->idlemax = -1;
  	config[i]->sessmax = -1;
  	config[i]->daymax = -1;
  	config[i]->warntime = 5;
  	config[i]->messages[IDLEMSG] = NULL;
  	config[i]->messages[SESSMSG] = NULL;
  	config[i]->messages[DAYMSG] = NULL;
  	config[i]->messages[NOLOGINMSG] = NULL;
	if ((tok = strsep(&lstart, ":")) != NULL) store_times(&config[i]->times, tok);
	if ((tok = strsep(&lstart, ":")) != NULL) alloc_cp(&config[i]->ttys, tok);
	if ((tok = strsep(&lstart, ":")) != NULL) alloc_cp(&config[i]->users, tok);
	if ((tok = strsep(&lstart, ":")) != NULL) alloc_cp(&config[i]->groups, tok);
	tok = strsep(&lstart, ":");
	if (tok != NULL && !strncasecmp(tok, "NOLOGIN", 7))
	{
		config[i]->login_allowed=0;
		if (tok[7] == ';') alloc_cp(&config[i]->messages[NOLOGINMSG], tok+8);
		else if ((tok = strsep(&lstart, ":")) != NULL) alloc_cp(&config[i]->messages[NOLOGINMSG], tok);
	}
	else
	if (tok != NULL && !strcasecmp(tok, "LOGIN")) config[i]->login_allowed=1;
	else
	{
		if (tok != NULL)
		{
		    config[i]->idlemax = atoi(tok);
		    if ((p = strchr(tok, ';')) != NULL) alloc_cp(&config[i]->messages[IDLEMSG], p+1);
		}
		if ((tok = strsep(&lstart, ":")) != NULL)
		{
		    config[i]->sessmax = atoi(tok);
		    if ((p = strchr(tok, ';')) != NULL) alloc_cp(&config[i]->messages[SESSMSG], p+1);
	    	}
		if ((tok = strsep(&lstart, ":")) != NULL)
		{
		    config[i]->daymax = atoi(tok);
		    if ((p = strchr(tok, ';')) != NULL) alloc_cp(&config[i]->messages[DAYMSG], p+1);
		}
		if ((tok = strsep(&lstart, ":")) != NULL)
		{
		    config[i]->warntime = atoi(tok);
		}
	}
	if (!config[i]->times || !config[i]->ttys  ||
	    !config[i]->users || !config[i]->groups)
        {
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        	syslog(LOG_ERR,
        		"Error on line %d of config file (%s).  Line ignored.",
        		linenum, CONFIG);
        	closelog();
	}
	else i++;
      }
    }
    config[i] = NULL;

    if (fclose(config_file) == EOF)
      bailout("Cannot close config file", 1);

#ifdef DEBUG
	i = 0;
	while (config[i])
	{
		printf("line %d: ", i);
		j = 0;
	  	while (config[i]->times[j].days)
		  printf("%d(%d-%d):", config[i]->times[j].days,
		  			config[i]->times[j].starttime,
		  			config[i]->times[j].endtime),j++;
		printf("%s:%s:%s:%s:%d;%s:%d;%s:%d;%s:%d\n",
			config[i]->ttys,
			config[i]->users,
			config[i]->groups,
			config[i]->login_allowed?"LOGIN":"NOLOGIN",
			config[i]->idlemax,
			config[i]->messages[IDLEMSG] == NULL?"builtin":config[i]->messages[IDLEMSG],
			config[i]->sessmax,
			config[i]->messages[SESSMSG] == NULL?"builtin":config[i]->messages[SESSMSG],
			config[i]->daymax,
			config[i]->messages[DAYMSG] == NULL?"builtin":config[i]->messages[DAYMSG],
			config[i]->warntime
			),i++;
	}
printf("End debug output.\n");
#endif /* DEBUG */
}

char chktimes(te)
struct time_ent *te;
{
    while (te->days)
    {
        if (daynums[now.tm_wday] & te->days &&	/* Date within range */
              ((te->starttime <= te->endtime &&	/* Time within range */
               now_hhmm >= te->starttime &&	
               now_hhmm <= te->endtime)
               ||
               (te->starttime > te->endtime &&
               (now_hhmm >= te->starttime ||
                now_hhmm <= te->endtime))
              )
           )
               return 1;
        te++;
    }
    return 0;
}

char chkmatch(element, in_set)
char *element;
char *in_set;
{
    char	*t;
    char	*set = (char *) malloc(strlen(in_set) + 1);

    if (set == NULL) bailout("Out of memory", 1);
    else strcpy(set, in_set);

    t = strtok(set, " ,");
    while (t)
    {
        if (t[strlen(t)-1] == '*')
        {
            if (!strncmp(t, element, strlen(t) - 1))
            {
              free(set);
              return 1;
	    }
	}
        else if (!strcmp(t, element))
        {
          free(set);
          return 1;
	}
        t = strtok(NULL, " ,");
    }
    free(set);
    return 0;
}

/* Return the number of minutes which user has been logged in for on
 * any of the ttys specified in config[configline] during the current day.
 */

void get_day_time(user)
char *user;
{
    struct ut_list	*login_p;
    struct ut_list	*logout_p;
    struct ut_list	*prev_p;

    daytime = 0;
    login_p = wtmplist;
    while (login_p)
    {
        /* For each login on a matching tty find its logout */
        if (
#ifndef SUNOS
	    login_p->elem.ut_type == USER_PROCESS &&
#endif
            !strncmp(login_p->elem.ut_user, user, 8) &&
            chkmatch(login_p->elem.ut_line, config[configline]->ttys))
        {
#ifdef DEBUG_WTMP
    struct tm	*tm;
    tm = localtime(&(login_p->elem.ut_time));
    fprintf(stderr, "%d:%d %s %s %s\n", 
    	tm->tm_hour,tm->tm_min, login_p->elem.ut_line,
    	login_p->elem.ut_user,
    	"login");
#endif
            prev_p = logout_p = login_p->next;
            while (logout_p)
            {
/*
 * SA19931128
 * If there has been a crash, then be reasonably fair and use the
 * last recorded login/logout as the user's logout time.  This will
 * potentially allow them slightly more online time than usual,
 * but is better than marking them as logged in for the time the machine
 * was down.
 */
#ifndef SUNOS
                if (logout_p->elem.ut_type == BOOT_TIME)
                {
                      logout_p = prev_p;
                      break;
	        }
#endif
                if (/*logout_p->elem.ut_type == DEAD_PROCESS &&*/
                  !strcmp(login_p->elem.ut_line, logout_p->elem.ut_line))
                      break;
                prev_p = logout_p;
                logout_p = logout_p->next;
            }

#ifdef DEBUG_WTMP
    if (logout_p)
    {
      tm = localtime(&(logout_p->elem.ut_time));
      fprintf(stderr, "%d:%d %s %s %s\n", 
    	tm->tm_hour,tm->tm_min, logout_p->elem.ut_line,
    	logout_p->elem.ut_user, "logout");
fprintf(stderr, "%s %d minutes\n", user, ((logout_p?logout_p->elem.ut_time:time_now) - login_p->elem.ut_time)/60);
    }
#endif
            daytime += (logout_p?logout_p->elem.ut_time:time_now) - login_p->elem.ut_time;
        }
        login_p = login_p->next;
    }
    daytime /= 60;
#ifdef DEBUG
fprintf(stderr, "%s has been logged in for %d minutes today.\n", user, daytime);
#endif
    return;
}

void warnpending(tty, time_remaining, user, host)
char *tty;
int time_remaining;
char *user;
char *host;
{
    int		fd;
    FILE	*ttyf;
    char 	cmdbuf[1024];

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Warning %s@%s on %s of pending logoff in %d minutes.",
    	user, host, tty, time_remaining);
    closelog();
#endif

    if(chk_xsession(tty, host)) {
    	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    	syslog(SYSLOG_DEBUG, "Warning %s running X on %s for pending logout! (%d min%s left)", user, tty, time_remaining, time_remaining==1?"":"s");
    	closelog();

  	/* then send the message using xmessage */
  	/* well, this is not really clean: */
  	sprintf(cmdbuf, "su %s -c \"xmessage -display %s -center 'WARNING: You will be logged out in %d minute%s when your %s limit expires.'&\"", user, tty, time_remaining, time_remaining==1?"":"s", limit_names[limit_type]);
  	system(cmdbuf);
  	/*#ifdef DEBUG*/
	    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
            syslog(LOG_DEBUG, "cmdbuf=%s", cmdbuf);
            closelog();
  	/*#endif*/
  	sleep(KWAIT); /* and give the user some time to read the message ;) */
	return;
    }
    
    if ((fd = open(tty, O_WRONLY|O_NOCTTY|O_NONBLOCK)) < 0 ||
	(ttyf = fdopen(fd, "w")) == NULL)
    {
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(LOG_ERR, "Could not open %s to warn of impending logoff.\n",tty);
        closelog();
        return;
    }
    fprintf(ttyf, "\r\nWARNING:\r\nYou will be logged out in %d minute%s when your %s limit expires.\r\n",
    	time_remaining, time_remaining==1?"":"s", limit_names[limit_type]);
    fclose(ttyf);
}

char chk_timeout(user, dev, host, idle, session)
char *user;
char *dev;
char *host;
int idle;
int session;
{
    struct passwd	*pw;
    struct group	*gr;
    struct group	*secgr;
    char	timematch = 0;
    char	ttymatch = 0;
    char	usermatch = 0;
    char	groupmatch = 0;
    char	*tty = dev;
    char	**p;
    int		disc;

    configline = 0;

/* Find primary group for specified user */
    if ((pw = getpwnam(user)) == NULL)
    {
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_ERR, "Could not get password entry for %s.", user);
      closelog();
      return 0;
    }
    if ((gr = getgrgid(pw->pw_gid)) == NULL)
    {
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_ERR, "Could not get group name for %s.", user);
      closelog();
      return 0;
    }

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "Checking user %s group %s tty %s.", user, gr->gr_name, tty);
    closelog();
#endif

/* Check to see if current user matches any entry based on tty/user/group */
    while (config[configline])
    {
    	timematch = chktimes(config[configline]->times);
        ttymatch = chkmatch(tty, config[configline]->ttys);
        usermatch = chkmatch(user, config[configline]->users);
        groupmatch = chkmatch(gr->gr_name, config[configline]->groups);
/* If the primary group doesn't match this entry, check secondaries */
	setgrent();
	while (!groupmatch && (secgr = getgrent()) != NULL)
	{
	    p = secgr->gr_mem;
	    while (*p && !groupmatch)
	    {
/*
printf("Group %s member %s\n", secgr->gr_name, *p);
*/
	    	if (!strcmp(*p, user))
	    	    groupmatch = chkmatch(secgr->gr_name, config[configline]->groups);
	    	p++;
	    }
/*
	    free(gr);
*/
	}
/*
	endgrent();
*/
/* If so, then check their idle, daily and session times in turn */
        if (timematch && ttymatch && usermatch && groupmatch)
        {
          get_day_time(user);
#ifdef DEBUG
	  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
	  syslog(SYSLOG_DEBUG, "Matched entry %d", configline);
	  syslog(SYSLOG_DEBUG, "Idle=%d (max=%d) Sess=%d (max=%d) Daily=%d (max=%d) warntime=%d", idle, config[configline]->idlemax, session, config[configline]->sessmax, daytime, config[configline]->daymax, config[configline]->warntime);
	  closelog();
#endif
	  disc = getdisc(dev, host);

	  limit_type = NOLOGINMSG;
	  if (!config[configline]->login_allowed)
	  	return NOLOGIN;

	  limit_type = IDLEMSG;
	  if (disc == N_TTY && config[configline]->idlemax > 0 && idle >= config[configline]->idlemax)
	  	return IDLEMAX;

	  limit_type = SESSMSG;
	  if (config[configline]->sessmax > 0 && session >= config[configline]->sessmax)
	  	return SESSMAX;

	  limit_type = DAYMSG;
	  if (config[configline]->daymax > 0 && daytime >= config[configline]->daymax)
	  	return DAYMAX;

/* If none of those have been exceeded, then warn users of upcoming logouts */
	  limit_type = DAYMSG;
	  if (config[configline]->daymax > 0 && daytime >= config[configline]->daymax - config[configline]->warntime)
	  	warnpending(dev, config[configline]->daymax - daytime, user, host);
	  else
	  {
	    limit_type = SESSMSG;
	    if (config[configline]->sessmax > 0 && session >= config[configline]->sessmax - config[configline]->warntime)
		warnpending(dev, config[configline]->sessmax - session, user, host);
	  }

/* Otherwise, leave the poor net addict alone */
          return ACTIVE;
        }
        configline++;
    }

/* If they do not match any entries, then they can stay on forever */
    return ACTIVE;
}

void check_idle()    /* Check for exceeded time limits & logoff exceeders */
{
    char        user[sizeof(utmpp->ut_user)];
    char	host[sizeof(utmpp->ut_host)];
    struct stat status, *pstat;
    time_t	idle, sesstime, time();
    short aktconfigline = -1; /* -1 if user is in config; >0 if he's not in config, * is handled in an other way */

    pstat = &status;    /* point to status structure */
#ifndef SUNOS
    if (utmpp->ut_type != USER_PROCESS || !utmpp->ut_user[0]) /* if not user process */
        return;                      /* skip the utmp entry */
#endif
    strncpy(user, utmpp->ut_user, sizeof(user) - 1);   /* get user name */
    user[sizeof(user) - 1] = '\0';   /* null terminate user name string */


    /* Only check user if he is mentioned in the config */
    
    if(!config[0])
    	return; /* no entries in config */
    while(config[++aktconfigline] && aktconfigline >= 0)
    	if(strcmp(config[aktconfigline]->users, user) == 0 || config[aktconfigline]->users[0] == '*') {
    		aktconfigline = -2; /* we found user or * in config, so he/they has/have restrictions */
		break;
	}
	
    if(aktconfigline > 0) { /* > 0 if user is in config */
#ifdef DEBUG
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
	syslog(SYSLOG_DEBUG, "User %s or * not in config -> No restrictions. Not checking %s on %s", user, user, dev);
	closelog();
#endif
	return; /* now, we return because the user beeing checked is not in config, so he has no restrictions */
    }


    strncpy(host, utmpp->ut_host, sizeof(host) - 1);	/* get host name */
    host[sizeof(host) - 1] = '\0';
    strncpy(dev, utmpp->ut_line, sizeof(dev) - 1);    /* get device name */
    dev[sizeof(dev) - 1] = '\0';
    if (stat(dev, pstat) && !chk_xsession(dev, host) == TIMEOUTD_XSESSION_LOCAL)   /* if can't get status for 
    port && if it's not a local Xsession*/
    {
        sprintf(errmsg, "Can't get status of user %s's terminal (%s)\n",
        	user, dev);
	/* bailout(errmsg, 1); MOH: is there a reason to exit here? */
	return; 
    }
    /* idle time is the lesser of:
     * current time less last access time OR
     * current time less last modified time
     */
#ifdef TIMEOUTDX11	
    if(chk_xsession(dev, host) && !chk_xterm(dev, host)) { /* check idle for Xsession, but not for xterm */
    	idle = get_xidle(user, dev) / 1000 / 60; /* get_xidle returns millisecs, we need mins */
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
	syslog(SYSLOG_DEBUG, "get_xidle(%s,%s) returned %dmins idle for %s.", dev, host, (int)idle, user);
	closelog();
    }
    else if (chk_xterm(dev, host)) return;
    else
#endif
    	idle = (time_now - max(pstat->st_atime, pstat->st_mtime)) / 60;

    sesstime = (time_now - utmpp->ut_time) / 60;
    switch(chk_timeout(user, dev, host, idle, sesstime))
    {
    	case ACTIVE:
#ifdef DEBUG
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(SYSLOG_DEBUG, "User %s is active.", user);
		closelog();
#endif
    		break;
    	case IDLEMAX:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    		syslog(LOG_NOTICE,
    		       "User %s exceeded idle limit (idle for %ld minutes, max=%d).\n",
    		       user, idle, config[configline]->idlemax);
    		closelog();
		killit(utmpp->ut_pid, user, dev, host);
    		break;
    	case SESSMAX:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_NOTICE,
		       "User %s exceeded maximum session limit at %s (on for %ld minutes, max=%d).\n",
		       user, dev, sesstime, config[configline]->sessmax);
    		closelog();
		killit(utmpp->ut_pid, user, dev, host);
    		break;
    	case DAYMAX:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_NOTICE,
		       "User %s exceeded maximum daily limit (on for %d minutes, max=%d).\n",
		       user, daytime, config[configline]->daymax);
    		closelog();
		killit(utmpp->ut_pid, user, dev, host);
    		break;
	case NOLOGIN:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		#ifdef DEBUG
		syslog(LOG_NOTICE, "NOLOGIN period reached for user %s@%s. (pid %d)", user, host, utmpp->ut_pid);
		#else
		syslog(LOG_NOTICE, "NOLOGIN period reached for user %s %s", user, host);
		#endif
		closelog();
		killit(utmpp->ut_pid, user, dev, host);
		break;
	default:
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_ERR, "Internal error - unexpected return from chk_timeout");
    		closelog();
    }
}

void bailout(message, status) /* display error message and exit */
int     status;     /* exit status */
char    *message;   /* pointer to the error message */
{
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(LOG_ERR, "Exiting - %s", message);
    closelog();
    exit(status);
}

void shut_down(signum)
int signum;
{
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(LOG_NOTICE, "Received SIGTERM.. exiting.");
    closelog();
    exit(0);
}

void segfault(signum)
int signum;
{
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(LOG_NOTICE, "Received SIGSEGV.. Something went wrong! Exiting!");
    closelog();
    exit(0);
}

void logoff_msg(tty)
int tty;
{
    FILE	*msgfile = NULL;
    char	msgbuf[1024];
    int		cnt;

    if (config[configline]->messages[limit_type])
    	msgfile = fopen(config[configline]->messages[limit_type], "r");

    if (msgfile)
    {
    	while ((cnt = read(tty, msgbuf, 1024)) > 0)
    	    write(tty, msgbuf, cnt);
    	fclose(msgfile);
    }
    else
    {
    	if (limit_type == NOLOGINMSG)
    	    sprintf(msgbuf, "\r\n\r\nLogins not allowed at this time.  Please try again later.\r\n");
    	else
    	    sprintf(msgbuf, "\r\n\r\nYou have exceeded your %s time limit.  Logging you off now.\r\n\r\n", limit_names[limit_type]);
    	write(tty, msgbuf, strlen(msgbuf));
    }
}

/* terminate process using SIGHUP, then SIGKILL */
void killit(pid, user, dev, host)
int pid;
char *user;
char *dev;
char *host;
{
    int	tty;
    pid_t cpid;
#ifdef SUNOS
   struct passwd	*pw;
#endif

    if(chk_xsession(dev, host) && !chk_xterm(dev, host)) {
	killit_xsession(utmpp->ut_pid, user, dev);
    	return;
    }
/* Tell user which limit they have exceeded and that they will be logged off */
    if ((tty = open(dev, O_WRONLY|O_NOCTTY|O_NONBLOCK)) < 0)
    {
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(LOG_ERR, "Could not write logoff message to %s.", dev);
        closelog();
	return;
    }
	
	
	/* check if the pid is sshd. If so, get PID of the child process (another ssh, owned by the user).
	   Test reverse if this child process is also ssh and owned by the user we want to log out.
	   (because we don't want to slay another user ;) */
	cpid = getcpid(pid);
#ifdef DEBUG
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
	syslog(LOG_NOTICE, "I am at killit() pid=%d user=%s child=%d line %d", pid, user, cpid, __LINE__);
	closelog();
#endif
	
	if(chk_ssh(pid) && chk_ssh(cpid) && !strcmp(getusr(cpid), user)) {
#ifdef DEBUG
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        	syslog(LOG_NOTICE, "User %s (pid:%d, cpid:%d) logged in via ssh from %s.", user, pid, cpid, host);
        	closelog();
#endif
		pid = cpid;
	}

    logoff_msg(tty);
    sleep (KWAIT); /*make sure msg does not get lost, again (esp. ssh)*/
    close(tty);

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(LOG_NOTICE, "Would normally kill pid %d user %s on %s",pid,user,dev);
    closelog();
    return;
#endif

    if (fork())             /* the parent process */
        return;            /* returns */

/* Wait a little while in case the above message gets lost during logout */
#ifdef SURE_KILL
    signal(SIGHUP, SIG_IGN);
    if ((pw = getpwnam(user)) == NULL)
    {
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(LOG_ERR, "Could not log user %s off line %s - unable to determine uid.", user, dev);
        closelog();
    }
    if (setuid(pw->pw_uid))
    {
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(LOG_ERR, "Could not log user %s off line %s - unable to setuid(%d).", user, dev, pw->pw_uid);
        closelog();
    }
    kill(-1, SIGHUP);
    sleep(KWAIT);
    kill(-1, SIGKILL);
#else
    kill(pid, SIGHUP);  /* first send "hangup" signal */
    sleep(KWAIT);
    if (!kill(pid, 0)) {    /* SIGHUP might be ignored */
        kill(pid, SIGKILL); /* then send sure "kill" signal */
        sleep(KWAIT);
        if (!kill(pid, 0))
        {
	    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
            syslog(LOG_ERR, "Could not log user %s off line %s.", user, dev);
            closelog();
        }
    }
#endif
    exit(0);
}

void reread_config(signum)
int signum;
{
    int i = 0;

    if (!allow_reread)
        pending_reread = 1;
    else
    {
        pending_reread = 0;
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        syslog(LOG_NOTICE, "Re-reading configuration file.");
        closelog();
        while (config[i])
        {
            free(config[i]->times);
            free(config[i]->ttys);
            free(config[i]->users);
            free(config[i]->groups);
            if (config[i]->messages[IDLEMSG]) free(config[i]->messages[IDLEMSG]);
            if (config[i]->messages[DAYMSG]) free(config[i]->messages[DAYMSG]);
            if (config[i]->messages[SESSMSG]) free(config[i]->messages[SESSMSG]);
            if (config[i]->messages[NOLOGINMSG]) free(config[i]->messages[NOLOGINMSG]);
            free(config[i]);
            i++;
        }
        read_config();
    }
    signal(SIGHUP, reread_config);
}

void reapchild(signum)
int signum;
{
    int st;

    wait(&st);
    signal(SIGCHLD, reapchild);
}

int getdisc(d, host)
char *d;
char *host;
{
    int	fd;
    int	disc;

#ifdef linux
    if(chk_xsession(d, host) || chk_xterm(d, host))
    	return N_TTY;
	
    if ((fd = open(d, O_RDONLY|O_NONBLOCK|O_NOCTTY)) < 0)
    {
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_WARNING, "Could not open %s for checking line discipline - idle limits will be enforced.", d);
      closelog();
      return N_TTY;
    }

    if (ioctl(fd, TIOCGETD, &disc) < 0)
    {
      close(fd);
      openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
      syslog(LOG_WARNING, "Could not get line discipline for %s - idle limits will be enforced.", d);
      closelog();
      return N_TTY;
    }

    close(fd);

#ifdef DEBUG
    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
    syslog(SYSLOG_DEBUG, "TTY %s: Discipline=%s.",d,disc==N_SLIP?"SLIP":disc==N_TTY?"TTY":disc==N_PPP?"PPP":disc==N_MOUSE?"MOUSE":"UNKNOWN");
    closelog();
#endif

    return disc;
#else
    return N_TTY;
#endif
}

int chk_xsession(dev, host) /* returns TIMEOUTD_XSESSION_{REMOTE,LOCAL,NONE} when dev and host seem to be a xSession. */
char *dev,*host;
{
    if(strncmp(dev, ":0", 1) == 0 && strlen(host) == 0 /*|| 
       (strncmp(dev, "pts/0", 3) == 0 && strncmp(host, ":0", 1) == 0    )*/) { /* if strings are the same, str[n]cmp returns 0 */
      /* Look here, how we check if it's a Xsession but no telnet or whatever.
       * The problem is that a xterm running on :0 has the device pts/?.  But if we ignore
       * all pts/?, ssh users won't be restricted.  
       * So, if (tty="pts/?" OR tty=":*") AND host = ":*", we have a Xsession:
       * 
       * seppy@schleptop:~$ w
       * 20:06:33 up 18 min,  6 users,  load average: 0.14, 0.16, 0.12
       * USER     TTY      FROM             LOGIN@   IDLE   JCPU   PCPU  WHAT
       * dennis   :0       -                19:48   ?xdm?   0.00s   ?     -
       * dennis   pts/1    :0.0             20:00    4:12   0.03s  0.03s  bash 
       * dennis   pts/2    :0.0             20:01    0.00s  0.18s  0.16s  ssh localhost 
       * dennis   pts/3    localhost        20:01    0.00s  0.01s  0.00s  w
       */       
      #ifdef DEBUG
         openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
         syslog(LOG_DEBUG, "LOCAL Xsession detected. device=%s host=%s", dev, host);
         closelog();
      #endif
	 return TIMEOUTD_XSESSION_LOCAL;
    }
    else if (strstr(dev, ":") && strlen(host) > 1 && gethostbyname(host)) {
      /* What about remote XDMCP sessions?
       * USER     TTY      FROM              LOGIN@   IDLE   JCPU   PCPU WHAT
       * mark     pts/3    mercury           Sat11    0.00s 10.99s  0.04s w
       * rebecca  ap:10    ap                10:32    0.00s  0.00s  1.28s x-session-manager
       */

      #ifdef DEBUG
         openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
         syslog(LOG_DEBUG, "REMOTE Xsession detected. device=%s host=%s", dev, host);
         closelog();
      #endif
      return TIMEOUTD_XSESSION_REMOTE;
    }
    else {
      #ifdef DEBUG
         openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
         syslog(LOG_DEBUG, "NO xsession detected. device=%s host=%s", dev, host);
         closelog();
      #endif
      return TIMEOUTD_XSESSION_NONE;
    }
}

/* We have to handle Xterms(pts/?) and Xsessions (:0) different:
   - Check Xsession for idle, but not a XTERM
   - Send message for pending logoff to X, but not to XTERM
     -> Don't check XTERM at all
   - but: check ssh (pts/?) but no XTERM (again)
*/
int chk_xterm(dev, host) /* returns 1 when dev and host seem to be a xTERM. */
char *dev,*host;
{
    if(strncmp(dev, "pts/0", 3) == 0 && strncmp(host, ":0", 1) == 0 ) {
      #ifdef DEBUG
         openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
         syslog(LOG_DEBUG, "XTERM detected. device=%s host=%s Ignoring.", dev, host);
         closelog();
      #endif
    	return 1;
    }
    else
    	return 0;
} /* chk_xterm(dev,host) */


void killit_xsession(pid, user, dev) /* returns 1 when dev and host seem to be a xSession. */
int pid;
char *dev, *user;
{
    char	msgbuf[1024], cmdbuf[1024];
  /* first, get the message into msgbuf */
    	if (limit_type == NOLOGINMSG) {
    	    sprintf(msgbuf, "Logins not allowed at this time.  Please try again later.");
	} else {
    	    sprintf(msgbuf, "You have exceeded your %s time limit.  Logging you off now.", limit_names[limit_type]);
        }

  /* then send the message using xmessage */
  /* well, this is not really clean: */
  sprintf(cmdbuf, "su %s -c \"xmessage -display %s -center '%s'&\"", user, dev, msgbuf);
  system(cmdbuf);
  #ifdef DEBUG
	    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
            syslog(LOG_DEBUG, "cmdbuf=%s", cmdbuf);
            closelog();
  #endif
  sleep(KWAIT); /* and give the user some time to read the message ;) */
	    

  #ifndef DEBUG	
  /* kill pid here */
    kill(pid, SIGTERM);  /* otherwise, X crashes */
    sleep(KWAIT);
    if (!kill(pid, 0)) {    /* SIGHUP might be ignored */
        kill(pid, SIGKILL); /* then send sure "kill" signal */
        sleep(KWAIT);
        if (!kill(pid, 0))
        {
	    openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
            syslog(LOG_ERR, "Could not log user %s off line %s. (running X)", user, dev);
            closelog();
        }
    }
  #else
  openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
  syslog(LOG_ERR, "Would normally logoff user %s running X (kill PID %d)", user, pid);
  closelog();
  #endif
}



int chk_ssh(pid)/* seppy; returns true if pid is sshd, otherwise it returns false */
pid_t pid;
{
	sprintf(path, "/proc/%d/stat", pid);
	proc_file = fopen(path, "r");
	if(!proc_file) {
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        	syslog(LOG_WARNING, "chk_ssh(): PID %d does not exist. Something went wrong. Ignoring.", pid);
        	closelog();
		return 0;
	}

	fscanf (proc_file, "%*d (%[^)]", comm);
	fclose(proc_file);
	
	if(!strcmp(comm, "sshd"))
		return 1;
	else
		return 0;
} 

char *getusr(pid) /*seppy; returns the name of the user owning process with the Process ID pid */
pid_t pid;
{
	char uid[99];
	sprintf(path, "/proc/%d/status", pid);
	proc_file = fopen(path, "r");
	if(!proc_file) {
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
        	syslog(LOG_NOTICE, "getusr(): PID %d does not exist. Ignoring.", pid);
        	closelog();
		return "unknown";
	}
	while(!fscanf(proc_file, "Uid:    %s", uid))
                fgets(uid, 98, proc_file);
	fclose(proc_file);
	return getpwuid(atoi(uid))->pw_name;
}

#ifdef TIMEOUTDX11
Time get_xidle(user, display) /*seppy; returns millicecs since last input event */
char *user;
char *display;
{
	Display* dpy;
	static XScreenSaverInfo* mitInfo = 0; 
	struct passwd *pwEntry;
	char homedir[50]; /*50 should be enough*/
	char oldhomedir[50];
	uid_t oldeuid;
	Time retval = 0;

	pwEntry = getpwnam(user);
	if(!pwEntry) {
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_ERR, "Could not get passwd-entry for user %s", user);
		closelog();
	}

#ifdef DEBUG
	openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
	syslog(LOG_DEBUG, "su-ing to %s(%d) and connecting to X", user, pwEntry->pw_uid);
	closelog();
#endif

	/*change into the user running x. we need that to connect to X*/
	/*setregid(1000, 1000); We don't need this*/

	/*save old, to come back*/
	oldeuid = geteuid();
	sprintf(oldhomedir, "HOME=%s", getenv("HOME"));

	/*become user*/
	if(seteuid(pwEntry->pw_uid) == -1) {
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_ERR, "Could not seteuid(%d).", pwEntry->pw_uid);
		closelog();
	}

	sprintf(homedir, "HOME=%s", pwEntry->pw_dir);
	putenv(homedir);

	/* First, check if there is a xserver.. */ 
	if ((dpy = XOpenDisplay (display)) == NULL) { /* = intended */
		openlog("timeoutd", OPENLOG_FLAGS, LOG_DAEMON);
		syslog(LOG_NOTICE, "Could not connect to %s to query idle-time for %s. Ignoring.", display, user);
		closelog();
	} else {
	if (!mitInfo) 
	  mitInfo = XScreenSaverAllocInfo ();
	XScreenSaverQueryInfo (dpy, DefaultRootWindow (dpy), mitInfo);
	retval = mitInfo->idle;
	XCloseDisplay(dpy);
	}
	/*go back again*/
	putenv(oldhomedir);
	setuid(oldeuid);

    	return retval;
} /* get_xidle(user) */
#endif


/* seppy; getchild()
          returns the pid of the first child-process found. 
          - 1 if a error occured, 
	  - 0 if none found
	  
	  We need this because utmp returns a process owned by 
	  root when a user is connected via ssh. If we kill its
	  child (owned by the user) he/she gets logged off */
pid_t getcpid(ppid) 
pid_t ppid;
{
	DIR *proc;
	FILE *proc_file;
	struct dirent *cont;
	char akt_pid[99];
	char path[256];
	
	proc = opendir("/proc/");
	if(proc == NULL) {
		printf("error opening directory\n");
		return -1; /* error */
	}
		
	while((cont = readdir(proc)) != NULL)
		if(cont->d_type == 4 && isdigit(cont->d_name[0])) { /* check only PIDs */						
			sprintf(path, "/proc/%s/status", cont->d_name);
			proc_file = fopen(path, "r");
			if(!proc_file)
				printf("error opening proc status file %s\n", path);
				
			while(!fscanf(proc_file, "PPid:    %s", akt_pid))
                		fgets(akt_pid, 10, proc_file);
			
			if(atoi(akt_pid) == ppid)
				return (pid_t)atoi(cont->d_name); /* return pid of child */
		} /* if(cont->d_type == 4) */
		
	return 0; /* no child found */	
} /* getchild(ppid) */
