/*
 *
 * /var/run/console.lock is the file used to control access to
 * devices.  It is created when the first console user logs in,
 * and that user has the control of the console until they have
 * logged out of all concurrent login sessions.  That is,
 * user A logs in on console 1 (gets access to console devices)
 * user B logs in on console 2 (does not get access)
 * user A logs in on console 3 (already has access)
 * user A logs out of console 1 (still has access on console 3)
 * user A logs out of console 3 (access revoked; user B does NOT get access)
 * Note that all console users (both A and B in this situation)
 * should be able to run console access programs (that is,
 * pam_sm_authenticate() should return PAM_SUCCESS) even if
 * console access to files/devices is not available to any one of
 * the users (B in this case).
 *
 * /var/run/console/<username> is used for reference counting
 * and to make console authentication easy -- if it exists, then
 * <username> has console access.
 *
 * A system startup script should remove /var/run/console.lock
 * and everything in /var/run/console/
 */

#include "../config.h"
#include "../lib/libmisc.h"
#include <errno.h>
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "pam_console.h"

#include <security/pam_modules.h>
#include <security/_pam_macros.h>

/* In order to avoid errors in pam_get_item(), we need a very unfortunate
 * cast.  This is a terrible design error in Linux-PAM which creates
 * source-level incompatibilities with other PAM implementations.  :-(
 */
#define CAST_ME_HARDER (const void**)

static char consolelock[PATH_MAX] = LOCKDIR "/console.lock";
static char consolerefs[PATH_MAX] = LOCKDIR "/console/";
static char consoleapps[PATH_MAX] = "/etc/security/console.apps/";
static char consoleperms[PATH_MAX] = "/etc/security/console.perms";
static int configfileparsed = 0;
static int debug = 0;
static int allow_nonroot_tty = 0;

/* some syslogging */

void
_pam_log(int err, int debug_p, const char *format, ...)
{
    va_list args;

    if (debug_p && !debug) return;

    va_start(args, format);
    openlog("pam_console", LOG_CONS|LOG_PID, LOG_AUTHPRIV);
    vsyslog(err, format, args);
    va_end(args);
    closelog();
}

void *
_do_malloc(size_t req)
{
  void *ret;

  ret = malloc(req);
  if (!ret) abort();
  return ret;
}

static void
_args_parse(int argc, const char **argv)
{
    for (; argc-- > 0; ++argv) {
	if (!strcmp(*argv,"debug"))
	    debug = 1;
	else if (!strcmp(*argv,"allow_nonroot_tty"))
	    allow_nonroot_tty = 1;
	else if (!strncmp(*argv,"permsfile=",10))
	    strcpy(consoleperms,*argv+10);
	else if (!strncmp(*argv,"fstab=",6))
	    chmod_set_fstab(*argv+6);
	else {
	    _pam_log(LOG_ERR, FALSE,
		     "_args_parse: unknown option; %s",*argv);
	}
    }
}

static int
is_root(pam_handle_t *pamh, const char *username) {
    /* this should correspond to suser() in the kernel, since the
     * whole point of this is to avoid doing unnecessary file ops
     */
    struct passwd *pwd;

    pwd = libmisc_getpwnam(pamh, username);
    if (pwd == NULL) {
        _pam_log(LOG_ERR, FALSE, "getpwnam failed for %s", username);
        return 0;
    }
    return !pwd->pw_uid;
}

static int
lock_console(const char *id)
{
    int fd, ret_val;

    fd = open(consolelock, O_RDWR|O_CREAT|O_EXCL, 0600);
    if (fd < 0) {
	_pam_log(LOG_INFO, TRUE,
		 "console file lock already in place %s", consolelock);
	return -1;
    }
    ret_val = libmisc_retry_write(fd, id, strlen(id)) != strlen(id);
    close(fd);
    if (ret_val == -1) {
        unlink(consolelock);
        return -1;
    }
    return 0;
}

/* warning, the following function uses goto for error recovery.
 * If you can't stand goto, don't read this function.  :-P
 */
static int
use_count(char *filename, int increment, int delete)
{
    int fd, err, val;
    static int cache_fd = 0;
    struct stat st;
    struct flock lockinfo;
    char *buf = NULL;

    if (cache_fd) {
	fd = cache_fd;
	cache_fd = 0;
	/* the cached fd is always already locked */
    } else {
top:
	fd = open(filename, O_RDWR|O_CREAT, 0600);
    	if (fd < 0) {
	    _pam_log(LOG_ERR, FALSE,
		    "Could not open lock file %s, disallowing console access",
		    filename);
	    return -1;
	}

	lockinfo.l_type = F_WRLCK;
	lockinfo.l_whence = SEEK_SET;
	lockinfo.l_start = 0;
	lockinfo.l_len = 0;
	alarm(20);	/* FIXME: what if caller has sigalrm masked? */
	err = fcntl(fd, F_SETLKW, &lockinfo);
	alarm(0);
	if (err == EAGAIN) {
	    /* if someone has locked the file and not written to it in
	     * at least 20 seconds, we assume they either forgot to unlock
	     * it or are catatonic -- chances are slim that they are in
	     * the middle of a read-write cycle and I don't want to make
	     * us lock users out.  Perhaps I should just return PAM_SUCCESS
	     * instead and log the event?  Kill the process holding the
	     * lock?  Options abound...  For now, we ignore it.
	     */
	    fcntl(fd, F_GETLK, &lockinfo);
	    /* now lockinfo.l_pid == 0 implies that the lock was released
	     * by the other process between returning from the 20 second
	     * wait and calling fcntl again, not likely to ever happen, and
	     * not a problem other than cosmetics even if it does.
	     */
	    _pam_log(LOG_ERR, FALSE,
		    "ignoring stale lock on file %s by process %d",
		    lockinfo.l_pid, filename);
	}

	/* it is possible at this point that the file has been removed
	 * by a previous login; if this happens, we need to start over.
	 * Unfortunately, the only way to do this without potential stack
	 * trashing is a goto.
	 */
	if (access (filename, F_OK) < 0) {
	    close (fd);
	    goto top;
	}
    }


    if (fstat (fd, &st)) {
	_pam_log(LOG_ERR, FALSE,
		"\"impossible\" fstat error on open fd for %s", filename);
	err = -1; goto return_error;
    }
    buf = _do_malloc(st.st_size+2); /* size will never grow by more than one */
    if (st.st_size) {
	if (libmisc_retry_read(fd, buf, st.st_size) != st.st_size) {
	    _pam_log(LOG_ERR, FALSE,
		    "\"impossible\" read error on %s", filename);
	    err = -1; goto return_error;
	}
	if (lseek(fd, 0, SEEK_SET) == -1) {
	    _pam_log(LOG_ERR, FALSE,
		    "\"impossible\" lseek error on %s", filename);
	    err = -1; goto return_error;
	}
	buf[st.st_size] = '\0';
        val = atoi(buf);
    } else {
	val = 0;
    }

    if (increment) { /* increment == 0 implies query */
	val += increment;
	if (val <= 0 && delete) {
	    if (unlink (filename)) {
		_pam_log(LOG_ERR, FALSE,
			"\"impossible\" unlink error on %s", filename);
		err = -1; goto return_error;
	    }
	    err = 0; goto return_error;
	}

	sprintf(buf, "%d", val);
	if (libmisc_retry_write(fd, buf, strlen(buf)) != strlen(buf)) {
	    _pam_log(LOG_ERR, FALSE,
		    "\"impossible\" write error on %s", filename);
	    err = -1; goto return_error;
	}
    }

    err = val;

    if (!increment) {
	cache_fd = fd;
    } else {
return_error:
	close (fd);
    }
    if (buf) free (buf);
    return err;
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  /* getuid() must return an id that maps to a username as a filename in
   * /var/run/console/
   * and the service name must be listed in
   * /etc/security/console-apps
   */
    struct passwd *pwd;
    char *lockfile = NULL;
    char *appsfile = NULL;
    const char *service;
    int ret = PAM_AUTH_ERR;

    D(("called."));
    _args_parse(argc, argv);

    /* FIXME: this appears to be unnecessary, since I always see pam_rootok
     * listed before this module -- remove if not explicitly required by the
     * pam_console white paper */
    if (getuid() == 0) return PAM_SUCCESS; /* root always trivially succeeds */

    pwd = libmisc_getpwuid(pamh, getuid());
    if (pwd == NULL) {
	_pam_log(LOG_ERR, FALSE, "user with id %d not found", getuid());
	return PAM_AUTH_ERR;
    }

    lockfile = _do_malloc(strlen(consolerefs) + strlen(pwd->pw_name) + 2);
    sprintf(lockfile, "%s%s", consolerefs, pwd->pw_name); /* trusted data */

    libmisc_get_string_item(pamh, PAM_SERVICE, &service);
    appsfile = _do_malloc(strlen(consoleapps) + strlen(service) + 2);
    sprintf(appsfile, "%s%s", consoleapps, service); /* trusted data */

    if (access(lockfile, F_OK) < 0) {
	_pam_log(LOG_ERR, TRUE,
		 "user %s not a console user", pwd->pw_name);
	ret = PAM_AUTH_ERR; goto error_return;
    }

    if (access(appsfile, F_OK) < 0) {
	_pam_log(LOG_ERR, TRUE,
		 "console access disallowed for service %s", service);
	ret = PAM_AUTH_ERR; goto error_return;
    }

    /* all checks OK, must be OK */
    ret = PAM_SUCCESS;

error_return:
    if (lockfile) free (lockfile);
    if (appsfile) free (appsfile);
    return ret;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  /* Create /var/run/console.lock if it does not exist
   * Create /var/run/console/<username> if it does not exist
   * Increment its use count
   * Change file ownerships and permissions as given in
   * /etc/security/console.perms IFF returned use count was 0
   * and we created /var/run/console.lock
   */
    int got_console = 0;
    int count = 0;
    int ret = PAM_SESSION_ERR;
    const char *username, *user_prompt;
    char *lockfile;
    const char *tty;

    D(("called."));
    _pam_log(LOG_ERR, TRUE, "pam_console open_session");
    _args_parse(argc, argv);
    user_prompt = "login: ";
    libmisc_get_string_item(pamh, PAM_USER_PROMPT, &user_prompt);
    username = NULL;
    pam_get_user(pamh, &username, user_prompt);
    _pam_log(LOG_DEBUG, TRUE, "user is \"%s\"",
	     username ? username : "(null)");
    if (!username || !username[0]) {
        _pam_log(LOG_DEBUG, TRUE, "user is \"%s\"",
	         username ? username : "(null)");
	return PAM_SESSION_ERR;
    }
    if (is_root(pamh, username)) {
        _pam_log(LOG_DEBUG, TRUE, "user \"%s\" is root", username);
	return PAM_SUCCESS;
    }
    tty = NULL;
    libmisc_get_string_item(pamh, PAM_TTY, &tty);
    if (!tty || !tty[0]) {
        _pam_log(LOG_ERR, TRUE, "TTY not defined");
	return PAM_SESSION_ERR;
    }

    /* get configuration */
    if (!configfileparsed) { parse_file(consoleperms); configfileparsed = 1; }

    /* return success quietly if not a terminal login */
    if (!check_console_name(tty, allow_nonroot_tty)) return PAM_SUCCESS;

    if (!lock_console(username)) got_console = 1;

    lockfile = _do_malloc(strlen(consolerefs) + strlen(username) + 2);
    sprintf(lockfile, "%s%s", consolerefs, username); /* trusted data */
    count = use_count(lockfile, 1, 0);
    if (count < 0) {
        ret = PAM_SESSION_ERR;
    } else {
        if (got_console) {
	    _pam_log(LOG_DEBUG, TRUE, "%s is console user", username);
	    /* woohoo!  We got here first, grab ownership and perms... */
	    set_permissions(pamh, tty, username, allow_nonroot_tty);
	    /* errors will be logged and are not critical */
	    ret = PAM_SUCCESS;
	}
    }

    free(lockfile);
    return ret;
}

PAM_EXTERN int
pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  /* Get /var/run/console/<username> use count, leave it locked
   * If use count is now 1:
   *   If /var/run/console.lock contains <username>"
   *     Revert file ownerships and permissions as given in
   *     /etc/security/console.perms
   * Decrement /var/run/console/<username>, removing both it and
   *   /var/run/console.lock if 0, unlocking /var/run/console/<username>
   *   in any case.
   */
    int fd;
    int count = 0;
    int err;
    int delete_consolelock = 0;
    const char *username = NULL, *user_prompt;
    char *lockfile = NULL;
    char *consoleuser = NULL;
    const char *tty;
    struct stat st;

    D(("called."));
    _args_parse(argc, argv);
    user_prompt = "login: ";
    libmisc_get_string_item(pamh, PAM_USER_PROMPT, &user_prompt);
    pam_get_user(pamh, &username, user_prompt);

    if (!username || !username[0]) return PAM_SESSION_ERR;
    if (is_root(pamh, username)) return PAM_SUCCESS;
    tty = NULL;
    libmisc_get_string_item(pamh, PAM_TTY, &tty);
    if (!tty || !tty[0]) return PAM_SESSION_ERR;

    /* get configuration */
    if (!configfileparsed) { parse_file(consoleperms); configfileparsed = 1; }

    /* return success quietly if not a terminal login */
    if (!check_console_name(tty, allow_nonroot_tty)) return PAM_SUCCESS;

    lockfile = _do_malloc(strlen(consolerefs) + strlen(username) + 2);
    sprintf(lockfile, "%s%s", consolerefs, username); /* trusted data */
    count = use_count(lockfile, 0, 0);
    if (count < 0) {
	err = PAM_SESSION_ERR; goto return_error;
    }

    if (count == 1) {
	fd = open(consolelock, O_RDONLY);
	if (fd != -1) {
	    if (fstat (fd, &st)) {
		_pam_log(LOG_ERR, FALSE,
			"\"impossible\" fstat error on %s", consolelock);
		err = PAM_SESSION_ERR; close(fd); goto return_error;
	    }
	    consoleuser = _do_malloc(st.st_size+1);
	    if (st.st_size) {
		if (libmisc_retry_read(fd, consoleuser, st.st_size) != st.st_size) {
		    _pam_log(LOG_ERR, FALSE,
			    "\"impossible\" read error on %s", consolelock);
		    err = PAM_SESSION_ERR; close(fd); goto return_error;
		}
		consoleuser[st.st_size] = '\0';
	    }
	    close (fd);

	    if (!strcmp(username, consoleuser)) {
		delete_consolelock = 1;
		reset_permissions(pamh, tty, allow_nonroot_tty);
		/* errors will be logged and at this stage we cannot do
		 * anything about them...
		 */
	    }
	} else {
	    /* didn't open file */
	    err = PAM_SESSION_ERR; goto return_error;
	}
    }

    count = use_count(lockfile, -1, 1);
    if (count < 1 && delete_consolelock) {
	if (unlink(consolelock)) {
	    _pam_log(LOG_ERR, FALSE,
		     "\"impossible\" unlink error on %s", consolelock);
	    err = PAM_SESSION_ERR; goto return_error;
	}
    }

    err = PAM_SUCCESS;
return_error:
    if (lockfile) free(lockfile);
    if (consoleuser) free (consoleuser);
    return err;
}

#ifdef PAM_STATIC

/* static module data */

struct pam_module _pam_console_modstruct = {
    "pam_console",
    pam_sm_authenticate,
    pam_sm_setcred,
    NULL,
    pam_sm_open_session,
    pam_sm_close_session,
    NULL,
};

#endif

/* end of module definition */
