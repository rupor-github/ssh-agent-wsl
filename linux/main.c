/*
 * ssh-agent-wsl Linux-side main code.
 * 
 * Based on weasel-pageant, Copyright 2017, 2018  Valtteri Vuorikoski
 * Based on ssh-pageant, Copyright 2009-2015  Josh Stone
 *
 * This file is part of ssh-agent-wsl, and is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */

#include <errno.h>
#include <err.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>  // needed by common.h

#include "../common.h"

// As of FCU (including earlier releases), a Win32 subprocess is in some
// sort of relationship with the conhost of the window in which it was started.
//
// Daemonizing breaks this, so disable it is disabled for now. ssh-agent-wsl remains
// attached to the tty that started it and exits when it goes away. The daemonization
// code is left here in case things improve in future Windows releases.
//#define REAL_DAEMONIZE 1

#define FD_FOREACH(fd, set) \
    for (fd = 0; fd < FD_SETSIZE; ++fd) \
        if (FD_ISSET(fd, set))

typedef enum {UNKNOWN, BOURNE, C_SH, FISH} shell_type;

struct fd_buf {
    ssize_t recv, send;
    uint8_t buf[AGENT_MAX_MSGLEN];
};

static int opt_debug = 0;
static int tty_gone = 0;
static int opt_no_exit = 0;

static pid_t subcommand_pid = 0;
static pid_t win32_pid = 0;
static int win32_in = -1;  // input from the win32 helper (connected to its stdout)
static int win32_out = -1;  // output to the win32 helper (connected to its stdin)
static char win32_helper_path[PATH_MAX] = "./pipe-connector.exe";

static char cleanup_tempdir[PATH_MAX] = "";
static char cleanup_sockpath[PATH_MAX] = "";


static void cleanup_exit(int status) __attribute__((noreturn));
static void cleanup_warn(const char *prefix) __attribute__((noreturn));
static void cleanup_signal(int sig);

static void do_agent_loop(int sockfd) __attribute__((noreturn));


static void
debug_print(const char *fmt, ...)
{
    if (!opt_debug)
        return;

    va_list ap;

    fprintf(stderr, "main DEBUG: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}


static void
cleanup_exit(int status)
{
    unlink(cleanup_sockpath);
    rmdir(cleanup_tempdir);
    exit(status);
}


static void
cleanup_warn(const char *prefix)
{
    warn("%s", prefix);
    cleanup_exit(1);
}


static void
cleanup_win32(int need_wait)
{
    if (win32_in > 0) {
        close(win32_in);
        win32_in = -1;
    }

    if (win32_out > 0) {
        close(win32_out);
        win32_out = -1;
    }

    // Used by the query function if it detects an EOF
    if (need_wait && win32_pid > 0)
        waitpid(win32_pid, NULL, 0);

    win32_pid = 0;
}


static int
wait_subcommand(int flags)
{
    int status = -1;

    if (waitpid(subcommand_pid, &status, flags) < 0)
        return status;

    if (WIFEXITED(status))
        status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        status = 128 + WTERMSIG(status);
    else
        status = 0;

    return status;
}


static void
cleanup_signal(int sig)
{
    // Most caught signals are basically just treated as exit notifiers,
    // but when a child exits, copy its exit status so ssh-agent-wsl is more
    // effective as a command wrapper.
    int status = 0;
    if (sig == SIGCHLD) {
        if (subcommand_pid > 0 && (status = wait_subcommand(WNOHANG)) >= 0) {
            // Fall through to exit.
        }
        else if (win32_pid > 0 && waitpid(win32_pid, NULL, WNOHANG) > 0) {
            // The win32 helper process exited. Clean up after it, the message handler
            // will restart it.
            cleanup_win32(0);
            return;
        }
        else {
            // This shouldn't happen. Exit in case subcommand tracking failed (this
            // is what we silently did before).
            fprintf(stderr, "received SIGCHLD for unknown child (subcommand_pid=%d win32_pid=%d)", subcommand_pid, win32_pid);
            status = 55;
        }
    }
    cleanup_exit(status);
}


// Create a temporary path for the socket.
static void
create_socket_path(char* sockpath, size_t len)
{
    char tempdir[] = "/tmp/ssh-XXXXXX";
    if (!mkdtemp(tempdir))
        cleanup_warn("mkdtemp");

    // NB: Don't set cleanup_tempdir until after it's created
    strncpy(cleanup_tempdir, tempdir, sizeof(cleanup_tempdir));

    snprintf(sockpath, len, "%s/agent.%d", tempdir, getpid());
}


// Prepare the socket at the given path.
static int
open_auth_socket(const char* sockpath)
{
    struct sockaddr_un addr;
    mode_t um;
    int fd;

    fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        cleanup_warn("socket");

    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path));
    addr.sun_family = AF_UNIX;

    um = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        cleanup_warn("bind");
    umask(um);

    // NB: Don't set cleanup_sockpath until after it's bound
    strncpy(cleanup_sockpath, sockpath, sizeof(cleanup_sockpath));

    if (listen(fd, 128) < 0)
        cleanup_warn("listen");

    return fd;
}


static int
path_is_socket(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISSOCK(st.st_mode))
            return 1;
    }
    return 0;
}


// Try to reuse an existing socket path.  For now, just being able to connect
// will be deemed good enough.  If it can't connect, but is still a socket, try
// to remove it.  Return 0 if the path was simply not connectible, else exit.
static int
reuse_socket_path(const char* sockpath)
{
    struct sockaddr_un addr;
    int fd;

    if (!sockpath[0])
        // No path
        return 0;

    fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        cleanup_warn("socket");

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        // The sockpath is already accepting connections -- reuse!
        close(fd);
        return 1;
    }
    else if (errno == ENOENT) {
        close(fd);
        debug_print("reuse_socket_path: socket %s not present", sockpath);
        return 0;
    }
    else if (errno == ECONNREFUSED) {
        // Either it's not listening, or not a socket at all.  If it was at
        // least a socket, remove it so it can be replaced.
        if (path_is_socket(sockpath)) {
            if (unlink(sockpath) < 0)
                cleanup_warn("unlink");

            close(fd);
            return 0;
        }

        debug_print("reuse_socket_path: socket %s not present", sockpath);

        // Restore the errno before warning out.
        errno = ECONNREFUSED;
    }
    cleanup_warn("connect");
}


static int
start_win32_helper()
{
    posix_spawn_file_actions_t action;
    int out_pipe[2], in_pipe[2];
    char child_arg[9];
    char *argv[] = { win32_helper_path, child_arg, NULL };
    char *cwd;
    int result = 0;

    if (win32_in >= 0 && win32_out >= 0)
        return result;  // already running

    // Serialize flags to child
    int child_flags = 0;
    if (opt_debug)
        child_flags |= WSLP_CHILD_FLAG_DEBUG;
    snprintf(child_arg, 9, "%08d", child_flags);

    // Set up the pipes to be used as stdin/stdout
    if (pipe2(out_pipe, O_CLOEXEC) < 0 || pipe2(in_pipe, O_CLOEXEC) < 0)
        cleanup_warn("start_win32_helper pipe");

    win32_in = in_pipe[0];  // our input, helper's output
    win32_out = out_pipe[1];  // our output, helper's input

    posix_spawn_file_actions_init(&action);

    // Set up stdin/stdout. The original files will be closed at exec.
    posix_spawn_file_actions_adddup2(&action, in_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&action, out_pipe[0], STDIN_FILENO);

    // Change to (hopefully) a DrvFs filesystem. Otherwise a warning about changing
    // the directory may be shown. In the future, this should check /proc/mounts for
    // a DrvFs path instead of assuming the C: drive is present.
    if ((cwd = get_current_dir_name()) != NULL) {
        if (chdir("/mnt/c") < 0)
            debug_print("could not chdir to DrvFs (%d)\n", errno);
    }

    // Start it
    if (posix_spawn(&win32_pid, win32_helper_path, &action, NULL, argv, environ) != 0) {
        // Display warning and clean up instead of exiting, in case the user is updating the helper
        warn("start_win32_helper failed to start helper %s", win32_helper_path);
        cleanup_win32(0);
        result = -1;
    }

    // Restore the original working directory. It would be nice if spawn() supported
    // this directly.
    if (cwd != NULL) {
        if (chdir(cwd) < 0)
            warn("failed to restore cwd");
        free(cwd);
    }

    // Close the files passed to the child.
    close(in_pipe[1]);
    close(out_pipe[0]);
    posix_spawn_file_actions_destroy(&action);

    if (result == 0) {
        // Read the initialization byte from the child to verify that it has started
        ssize_t initcnt;
        char initchar;

        initcnt = read(win32_in, &initchar, 1);
        if (initcnt < 0)
            cleanup_warn("could not read init byte from win32 helper");
        else if (initcnt == 0) {
            warnx("win32 helper died immediately");
            cleanup_exit(1);
        }
        else if (initchar != 'a') {
            warnx("win32 helper returned unexpected init byte %x", initchar);
            cleanup_exit(1);
        }
        debug_print("got init byte %x='%c'", initchar, initchar);
    }

    return result;
}


static int
agent_query(void *buf)
{
    if (start_win32_helper() != 0)
        return -1;

    // Subprocess has been started (though it may still fail, but at least the spawn finished)

    size_t rem = msglen(buf);
    ssize_t cnt;
    void *bufp = buf;
    int first_done = 0;

    while (rem > 0) {
        cnt = write(win32_out, bufp, rem);
        if (cnt < 0) {
            switch (errno) {
            case EINTR:
                if (win32_out < 0)
                    return -1;  // helper had died and signal handler cleaned up
                continue;

            case EPIPE:
                // The helper has closed its input; try to restart
                cleanup_win32(1);
                if (!first_done) {
                    warn("win32 helper had exited; trying to restart");
                    if (start_win32_helper() != 0)
                        return -1;
                    first_done = 1;  // not actually done, but don't retry infinitely
                    continue;
                }
                warn("win32 helper exited during query (write); aborting");
                return -1;

            default:
                // These shouldn't happen, bail completely
                cleanup_warn("agent_query write");
                break;
            }
        }

        // Write succeeded
        first_done = 1;
        rem -= (size_t) cnt;
        bufp += cnt;
    }

    first_done = 0;
    rem = 4; // start with 4-byte length
    bufp = buf;
    while (rem > 0) {
        cnt = read(win32_in, bufp, rem);
        if (cnt < 0) {
            switch (errno) {
            case EINTR:
                if (win32_in < 0)
                    return -1;  // helper had died and signal handler cleaned up
                continue;

            default:
                cleanup_warn("agent_query read");
                break;
            }
        }
        else if (cnt == 0) {
            // End of file on pipe, the helper went away
            warn("win32 helper exited during query (read, rem=%llu); aborting", rem);
            cleanup_win32(1);
            return -1;
        }

        rem -= (size_t) cnt;
        bufp += cnt;

        if (!first_done) {
            // Waiting for body size, see if we have it
            if (rem != 0)
                continue;  // nope

            rem = msglen(buf) - 4;  // yep, reset remaining to body size
            if (rem > (AGENT_MAX_MSGLEN - 4)) {  // dummy size
                warn("win32 helper tried to return %llu bytes; aborting", rem);
                cleanup_win32(1);
                return -1;
            }
            first_done = 1;
        }
    }

    return 0;
}


static int
agent_recv(int fd, struct fd_buf *p)
{
    ssize_t len = recv(fd, p->buf + p->recv, sizeof(p->buf) - (size_t)p->recv, 0);
    if (len <= 0) {
        if (len < 0)
            warn("recv(%d)", fd);
        return -1;
    }

    p->recv += len;
    if (p->recv < 4 || p->recv < msglen(p->buf))
        return 0;  // more to recv

    if (p->recv > msglen(p->buf)) {
        warnx("recv(%d) = %d (expected %d)",
              fd, p->recv, msglen(p->buf));
        return -1;
    }

    // Pass query to Windows ssh-agent
    if (agent_query(p->buf) != 0)
        return -1;

    p->send = 0;
    return 1;  // recv done, move to send phase
}


static int
agent_send(int fd, struct fd_buf *p)
{
    ssize_t len = send(fd, p->buf + p->send, (size_t)(msglen(p->buf) - p->send), 0);
    if (len < 0) {
        warn("send(%d)", fd);
        return -1;
    }

    p->send += len;
    if (p->send < msglen(p->buf))
        return 0;  // more to send

    if (p->send > msglen(p->buf)) {
        warnx("send(%d) = %d (expected %d)",
              fd, p->send, msglen(p->buf));
        return -1;
    }

    p->recv = 0;
    return 1;
}


// Two WSL problems require us to use a weird pseudo-daemon mode:
//  1. Detaching from the parent terminal breaks Win32 process communication
//  2. Session members are not sent a SIGHUP when the controlling terminal goes away (may be fixed post-FCU)
// Therefore, we need this one weird hack where we keep checking if our
// controlling terminal is gone. If it is, exit since talking to the
// helper would hang.
static void
check_tty_gone()
{
#if !REAL_DAEMONIZE
    if (tty_gone && opt_no_exit)
        return;
    int fd = open("/dev/tty", O_RDONLY);
    if (fd < 0) {
        if (errno == ENOTTY) {
            if (opt_no_exit) {
                // Controlling terminal is gone, kill the helper process so the parent conhost can exit
                if (win32_pid > 0 && kill(win32_pid, SIGTERM) < 0)
                    err(1, "kill(%d)", win32_pid);
                tty_gone = 1;
            }
            else {
                // Controlling terminal is gone
                cleanup_exit(0);
            }
        }
        else
            warn("checking controlling terminal failed");
    }
    else
        // We are still attached to a terminal
        close(fd);
#endif
}

static void
do_agent_loop(int sockfd)
{
    int fd;
    fd_set read_set, write_set;
    struct fd_buf *bufs[FD_SETSIZE] = { NULL };

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_SET(sockfd, &read_set);

    while (1) {
        fd_set do_read_set = read_set;
        fd_set do_write_set = write_set;
#if REAL_DAEMONIZE
        struct timeval *timeoutp = NULL;
#else
        struct timeval timeout = { 1, 0 }, *timeoutp = &timeout;
#endif
        int ready_fds;

        if ((ready_fds = select(FD_SETSIZE, &do_read_set, &do_write_set, NULL, timeoutp)) < 0) {
            if (errno == EINTR)
                continue;
            else
                cleanup_warn("select");
        }

        if (ready_fds == 0) {
            // select timed out
            check_tty_gone();
            continue;
        }

        if (FD_ISSET(sockfd, &do_read_set)) {
            int s = accept4(sockfd, NULL, NULL, SOCK_CLOEXEC);
            if (s >= FD_SETSIZE) {
                warnx("accept: Too many connections");
                close(s);
            }
            else if (s < 0)
                warn("accept");
            else {
                bufs[s] = calloc(1, sizeof(struct fd_buf));
                if (!bufs[s]) {
                    warnx("calloc: No memory");
                    close(s);
                }
                else
                    FD_SET(s, &read_set);
            }
            FD_CLR(sockfd, &do_read_set);
        }

        FD_FOREACH(fd, &do_read_set) {
            int res = agent_recv(fd, bufs[fd]);
            if (res != 0) {
                FD_CLR(fd, &read_set);
                if (res < 0) {
                    close(fd);
                    free(bufs[fd]);
                    bufs[fd] = NULL;
                }
                else
                    FD_SET(fd, &write_set);
            }
        }

        FD_FOREACH(fd, &do_write_set) {
            int res = agent_send(fd, bufs[fd]);
            if (res != 0) {
                FD_CLR(fd, &write_set);
                if (res < 0) {
                    close(fd);
                    free(bufs[fd]);
                    bufs[fd] = NULL;
                }
                else
                    FD_SET(fd, &read_set);
            }
        }
    }
}


// Quote and escape a string for shell eval.
// Caller must free the result.
static char *
shell_escape(const char *s)
{
    // The pessimistic growth is *4, when every character is ' mapped to '\''.
    // (No need to be clever.)  Add room for outer quotes and terminator.
    size_t len = strlen(s);
    char *mem = calloc(len + 1, 4);
    if (!mem)
        return NULL;

    char c, *out = mem;
    *out++ = '\''; // open the string
    for (c = *s++; c; c = *s++) {
        if (c == '\'') {
            *out++ = '\''; // close,
            *out++ = '\\'; // escape
            *out++ = '\''; // the quote,
            *out++ = '\''; // reopen
        }
        else
            *out++ = c; // plain copy
    }
    *out++ = '\''; // close the string
    *out++ = '\0'; // terminate
    return mem;
}

// Feel free to add complex shell detection logic
static shell_type
get_shell_guess()
{
    shell_type detected_shell = BOURNE;
    char * shell_env = getenv("SHELL") ?: "";

    if (!!strstr(shell_env, "csh")) {
        detected_shell = C_SH;
    }

    return detected_shell;
}


static void
output_unset_env(const shell_type opt_sh)
{
    switch (opt_sh) {
        case C_SH:
            printf("unsetenv SSH_AUTH_SOCK;\n");
            printf("unsetenv SSH_AGENT_PID;\n");
            break;
        case BOURNE:
            printf("unset SSH_AUTH_SOCK;\n");
            printf("unset SSH_AGENT_PID;\n");
            break;
        case FISH:
            printf("set -e SSH_AUTH_SOCK;\n");
            printf("set -e SSH_AGENT_PID;\n");
            break;
        case UNKNOWN:
            break;
    }
}


static void
output_set_env(const shell_type opt_sh, const int p_set_pid_env, const char *escaped_sockpath, const pid_t pid)
{
    switch (opt_sh) {
        case C_SH:
            printf("setenv SSH_AUTH_SOCK %s;\n", escaped_sockpath);
            if (p_set_pid_env)
                printf("setenv SSH_AGENT_PID %d;\n", pid);
            break;
        case BOURNE:
            printf("SSH_AUTH_SOCK=%s; export SSH_AUTH_SOCK;\n", escaped_sockpath);
            if (p_set_pid_env)
                printf("SSH_AGENT_PID=%d; export SSH_AGENT_PID;\n", pid);
            break;
        case FISH:
            printf("set -x SSH_AUTH_SOCK %s;\n", escaped_sockpath);
            if (p_set_pid_env)
                printf("set -x SSH_AGENT_PID %d;\n", pid);
            break;
        case UNKNOWN:
            break;
    }
}

static shell_type
parse_shell_option(const char *shell_name)
{
    if (!strcasecmp(shell_name, "fish")) {
        return FISH;
    } else if (!strcasecmp(shell_name, "csh")) {
        return C_SH;
    } else if (!strcasecmp(shell_name, "sh") ||
               !strcasecmp(shell_name, "bourne")) {
        return BOURNE;
    } else {
        errx(1, "unrecognized shell \"%s\"", shell_name);
        return UNKNOWN;  // not reached
    }
}

int
main(int argc, char *argv[])
{
    char sockpath[PATH_MAX] = "";
    int sockpath_from_env = 0;
    static struct option long_options[] = {
        { "help", no_argument, 0, 'h' },
        { "version", no_argument, 0, 'v' },
        { "reuse", no_argument, 0, 'r' },
        { "helper", required_argument, 0, 'H' },
        { 0, 0, 0, 0 }
    };

    int sockfd = -1;

    int opt;
    int opt_quiet = 0;
    int opt_kill = 0;
    int opt_reuse = 0;
    int opt_lifetime = 0;
    char exec_dir[PATH_MAX];
    ssize_t exec_dir_len;
    shell_type opt_sh = get_shell_guess();

    exec_dir_len = readlink("/proc/self/exe", exec_dir, PATH_MAX - 1);
    if (exec_dir_len > 0) {
        exec_dir[exec_dir_len] = 0;
        strcpy(exec_dir, dirname(exec_dir));
    }
    else {
        strcpy(exec_dir, ".");
    }

    // Assume that the helper binary is next to the main executable
    snprintf(win32_helper_path, PATH_MAX, "%s/%s", exec_dir, "pipe-connector.exe");

    while ((opt = getopt_long(argc, argv, "+hvcsS:kdqa:rt:H:b",
                              long_options, NULL)) != -1)
        switch (opt) {
            case 'h':
                printf("Usage: %s [options] [command [arg ...]]\n", program_invocation_short_name);
                printf("Options:\n");
                printf("  -h, --help     Show this help.\n");
                printf("  -v, --version  Display version information.\n");
                printf("  -c             Generate C-shell commands on stdout.\n");
                printf("  -s             Generate Bourne shell commands on stdout.\n");
                printf("  -S SHELL       Generate shell command for \"bourne\", \"csh\", or \"fish\".\n");
                printf("  -k             Kill the current %s.\n", program_invocation_short_name);
                printf("  -d             Enable debug mode.\n");
                printf("  -q             Enable quiet mode.\n");
                printf("  -a SOCKET      Create socket on a specific path.\n");
                printf("  -b             Do not exit when tty closes (only use on Windows 10 version 1809 and newer).\n");
                printf("  -r, --reuse    Allow to reuse an existing -a SOCKET.\n");
                printf("  -H, --helper   Path to the Win32 helper binary (default: %s).\n", win32_helper_path);
                printf("  -t TIME        Limit key lifetime in seconds (not supported by Windows port of ssh-agent).\n");
                return 0;

            case 'v':
                printf("ssh-agent-wsl 2.3\n");
                printf("Based on weasel-pageant, copyright 2017, 2018  Valtteri Vuorikoski\n");
                printf("Based on ssh-pageant, copyright 2009-2014  Josh Stone\n");
                printf("License GPLv3+: GNU GPL version 3 or later"
                       " <http://gnu.org/licenses/gpl.html>.\n");
                printf("This is free software:"
                       " you are free to change and redistribute it.\n");
                printf("There is NO WARRANTY, to the extent permitted by law.\n");
                return 0;

            case 'c':
                opt_sh = C_SH;
                break;

            case 's':
                opt_sh = BOURNE;
                break;

            case 'S':
                opt_sh = parse_shell_option(optarg);
                break;

            case 'k':
                opt_kill = 1;
                break;

            case 'd':
                opt_debug = 1;
                break;

            case 'q':
                opt_quiet = 1;
                break;

            case 'a':
                if (strlen(optarg) + 1 > sizeof(sockpath))
                    errx(1, "socket address is too long");
                strcpy(sockpath, optarg);
                break;

            case 'r':
                opt_reuse = 1;
                break;

            case 't':
                opt_lifetime = 1;
                break;

            case 'H':
                if (realpath(optarg, win32_helper_path) == NULL)
                    err(1, "invalid helper path (use --helper to specify the Win32 helper path)");
                break;

            case 'b':
                opt_no_exit = 1;
                break;

            case '?':
                errx(1, "try --help for more information");
                break;

            default:
                // shouldn't get here
                errx(2, "getopt returned unknown code %#X", opt);
                break;
        }

    if (opt_kill) {
        pid_t pid;
        const char *pidenv = getenv("SSH_AGENT_PID");
        if (!pidenv)
            errx(1, "SSH_AGENT_PID not set, cannot kill agent");
        pid = atoi(pidenv);
        if (kill(pid, SIGTERM) < 0)
            err(1, "kill(%d)", pid);
        output_unset_env(opt_sh);
        if (!opt_quiet)
            if (!strcasecmp((const char*)program_invocation_short_name, "ssh-agent")) {
                // Make sure output is compatible with openssh
                printf("echo Agent pid %d killed;\n", pid);
            } else {
                printf("echo ssh-agent-wsl pid killed%d;\n", pid);
            }
        return 0;
    }

    if (opt_reuse && !sockpath[0])
    {
        // If a fixed socket path was not specified, check if there is
        // a socket set in the environment. Its validity will be checked
        // later.
        char *env_sockpath = getenv("SSH_AUTH_SOCK");
        if (env_sockpath && strlen(env_sockpath) < PATH_MAX) {
            strcpy(sockpath, env_sockpath);
            sockpath_from_env = 1;
        }
    }

    if (opt_lifetime && !opt_quiet)
        warnx("option is not supported by Windows port of ssh-agent -- t");

    // Preflight the helper path
    if (access(win32_helper_path, X_OK) < 0)
        errx(1, "file %s is not an executable; use --helper to specify the Win32 helper path", win32_helper_path);

    signal(SIGINT, cleanup_signal);
    signal(SIGHUP, cleanup_signal);
    signal(SIGTERM, cleanup_signal);
    signal(SIGPIPE, SIG_IGN);

    int p_sock_reused = opt_reuse && reuse_socket_path(sockpath);
    if (!p_sock_reused) {
        if (!sockpath[0] || sockpath_from_env)
            create_socket_path(sockpath, sizeof(sockpath));
        sockfd = open_auth_socket(sockpath);
    }

    // If the sockpath is actually reused, don't daemonize, don't set
    // SSH_AGENT_PID, and don't go into do_agent_loop(). Just set
    // SSH_AUTH_SOCK and exit normally.
    int p_daemonize = !(opt_debug || p_sock_reused);
    int p_set_pid_env = !p_sock_reused;

    if (optind < argc) {
        // Subcommand exeuction mode
        char * const* subargv = argv + optind;
        setenv("SSH_AUTH_SOCK", sockpath, 1);
        if (p_set_pid_env) {
            char pidstr[16];
            snprintf(pidstr, sizeof(pidstr), "%d", getpid());
            setenv("SSH_AGENT_PID", pidstr, 1);
        }
        if (!p_sock_reused)
            signal(SIGCHLD, cleanup_signal);

        // Have spawn clean up ignored signals
        posix_spawnattr_t sp_attr;
        sigset_t sp_sigs;
        sigemptyset(&sp_sigs);
        sigaddset(&sp_sigs, SIGPIPE);

        posix_spawnattr_init(&sp_attr);
        posix_spawnattr_setsigdefault(&sp_attr, &sp_sigs);
        posix_spawnattr_setflags(&sp_attr, POSIX_SPAWN_SETSIGDEF);

        if (posix_spawnp(&subcommand_pid, subargv[0], NULL, &sp_attr, subargv, environ) < 0)
            cleanup_warn(subargv[0]);

        posix_spawnattr_destroy(&sp_attr);
    }
    else {
        // Daemon mode
        pid_t pid;
        if (p_daemonize) {
            pid = fork();
        }
        else {
            // Run both of the following forks in the same process.
            pid = getpid();
        }

        if (pid < 0)
            cleanup_warn("fork");
        if (pid > 0) {
            char *escaped_sockpath = shell_escape(sockpath);
            if (!escaped_sockpath)
                cleanup_warn("shell_escape");
            output_set_env(opt_sh, p_set_pid_env, escaped_sockpath, pid);
            free(escaped_sockpath);
            if (p_set_pid_env && !opt_quiet)
                if (!strcasecmp((const char *)program_invocation_short_name, "ssh-agent")) {
                    // Make sure output is compatible with openssh
                    printf("echo Agent pid %d;\n", pid);
                } else {
                    printf("echo ssh-agent-wsl pid %d;\n", pid);
                }
            if (p_daemonize)
                return 0;
        }
#if REAL_DAEMONIZE // if !REAL_DAEMONIZE remain attached to the tty and die with it
        else if (setsid() < 0)
            cleanup_warn("setsid");
        else {
            fclose(stderr);
            // Set up SIGCHLD handler to catch the helper process exiting
            signal(SIGCHLD, cleanup_signal);
        }
#else
        // Detach from process group but not the session to keep the controlling
        // tty but avoid receiving the foreground process group's signals. See
        // comments for check_tty_gone on why these tricks are needed.
        else if (setpgid(0, 0) < 0)
            cleanup_warn("setpgid");
        else
            // Set up SIGCHLD handler to catch the helper process exiting
            signal(SIGCHLD, cleanup_signal);
#endif
    }

    // If we close stdin, Win32 processes fail to receive a (seemingly unrelated) pipe as their stdin.
    // Thus skip close until if/when this gets fixed in WSL.
#if REAL_DAEMONIZE
    fclose(stdin);
#endif
    // But for whatever reason closing stdout is fine
    fclose(stdout);

    int status = 0;
    if (!p_sock_reused)
        // Run main loop and wait for agent connections
        do_agent_loop(sockfd);
    else if (subcommand_pid > 0)
        // Reused socket in subcommand mode: 
        status = wait_subcommand(0);

    return status;
}
