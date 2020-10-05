/**
 * @file
 * Send email using sendmail
 *
 * @authors
 * Copyright (C) 2020 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page send_sendmail Send email using sendmail
 *
 * Send email using sendmail
 */

#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "mutt/lib.h"
#include "address/lib.h"
#include "config/lib.h"
#include "gui/lib.h"
#include "lib.h"
#include "context.h"
#include "format_flags.h"
#include "mutt_globals.h"
#include "muttlib.h"
#include "options.h"
#include "pager.h"
#ifdef USE_NNTP
#include "nntp/lib.h"
#endif
#ifdef HAVE_SYSEXITS_H
#include <sysexits.h>
#else
#define EX_OK 0
#endif

SIG_ATOMIC_VOLATILE_T SigAlrm; ///< true after SIGALRM is received

ARRAY_HEAD(SendmailArgs, const char *);

/**
 * alarm_handler - Async notification of an alarm signal
 * @param sig Signal, (SIGALRM)
 */
static void alarm_handler(int sig)
{
  SigAlrm = 1;
}

/**
 * send_msg - invoke sendmail in a subshell
 * @param[in]  path     Path to program to execute
 * @param[in]  args     Arguments to pass to program
 * @param[in]  msg      Temp file containing message to send
 * @param[out] tempfile If sendmail is put in the background, this points
 *                      to the temporary file containing the stdout of the
 *                      child process. If it is NULL, stderr and stdout
 *                      are not redirected.
 * @param[in]  wait_time How long to wait for sendmail, `$sendmail_wait`
 * @retval  0 Success
 * @retval >0 Failure, return code from sendmail
 */
static int send_msg(const char *path, struct SendmailArgs *args,
                    const char *msg, char **tempfile, int wait_time)
{
  sigset_t set;
  int st;

  mutt_sig_block_system();

  sigemptyset(&set);
  /* we also don't want to be stopped right now */
  sigaddset(&set, SIGTSTP);
  sigprocmask(SIG_BLOCK, &set, NULL);

  if ((wait_time >= 0) && tempfile)
  {
    struct Buffer *tmp = mutt_buffer_pool_get();
    mutt_buffer_mktemp(tmp);
    *tempfile = mutt_buffer_strdup(tmp);
    mutt_buffer_pool_release(&tmp);
  }

  pid_t pid = fork();
  if (pid == 0)
  {
    struct sigaction act, oldalrm;

    /* save parent's ID before setsid() */
    pid_t ppid = getppid();

    /* we want the delivery to continue even after the main process dies,
     * so we put ourselves into another session right away */
    setsid();

    /* next we close all open files */
    close(0);
#ifdef OPEN_MAX
    for (int fd = tempfile ? 1 : 3; fd < OPEN_MAX; fd++)
      close(fd);
#elif defined(_POSIX_OPEN_MAX)
    for (int fd = tempfile ? 1 : 3; fd < _POSIX_OPEN_MAX; fd++)
      close(fd);
#else
    if (tempfile)
    {
      close(1);
      close(2);
    }
#endif

    /* now the second fork() */
    pid = fork();
    if (pid == 0)
    {
      /* "msg" will be opened as stdin */
      if (open(msg, O_RDONLY, 0) < 0)
      {
        unlink(msg);
        _exit(S_ERR);
      }
      unlink(msg);

      if ((wait_time >= 0) && tempfile && *tempfile)
      {
        /* *tempfile will be opened as stdout */
        if (open(*tempfile, O_WRONLY | O_APPEND | O_CREAT | O_EXCL, 0600) < 0)
          _exit(S_ERR);
        /* redirect stderr to *tempfile too */
        if (dup(1) < 0)
          _exit(S_ERR);
      }
      else if (tempfile)
      {
        if (open("/dev/null", O_WRONLY | O_APPEND) < 0) /* stdout */
          _exit(S_ERR);
        if (open("/dev/null", O_RDWR | O_APPEND) < 0) /* stderr */
          _exit(S_ERR);
      }

      /* execvpe is a glibc extension */
      /* execvpe (path, args, mutt_envlist_getlist()); */
      execvp(path, (char **) args->entries);
      _exit(S_ERR);
    }
    else if (pid == -1)
    {
      unlink(msg);
      FREE(tempfile);
      _exit(S_ERR);
    }

    /* wait_time > 0: interrupt waitpid() after wait_time seconds
     * wait_time = 0: wait forever
     * wait_time < 0: don't wait */
    if (wait_time > 0)
    {
      SigAlrm = 0;
      act.sa_handler = alarm_handler;
#ifdef SA_INTERRUPT
      /* need to make sure waitpid() is interrupted on SIGALRM */
      act.sa_flags = SA_INTERRUPT;
#else
      act.sa_flags = 0;
#endif
      sigemptyset(&act.sa_mask);
      sigaction(SIGALRM, &act, &oldalrm);
      alarm(wait_time);
    }
    else if (wait_time < 0)
      _exit(0xff & EX_OK);

    if (waitpid(pid, &st, 0) > 0)
    {
      st = WIFEXITED(st) ? WEXITSTATUS(st) : S_ERR;
      if (wait_time && (st == (0xff & EX_OK)) && tempfile && *tempfile)
      {
        unlink(*tempfile); /* no longer needed */
        FREE(tempfile);
      }
    }
    else
    {
      st = ((wait_time > 0) && (errno == EINTR) && SigAlrm) ? S_BKG : S_ERR;
      if ((wait_time > 0) && tempfile && *tempfile)
      {
        unlink(*tempfile);
        FREE(tempfile);
      }
    }

    if (wait_time > 0)
    {
      /* reset alarm; not really needed, but... */
      alarm(0);
      sigaction(SIGALRM, &oldalrm, NULL);
    }

    if ((kill(ppid, 0) == -1) && (errno == ESRCH) && tempfile && *tempfile)
    {
      /* the parent is already dead */
      unlink(*tempfile);
      FREE(tempfile);
    }

    _exit(st);
  }

  sigprocmask(SIG_UNBLOCK, &set, NULL);

  if ((pid != -1) && (waitpid(pid, &st, 0) > 0))
    st = WIFEXITED(st) ? WEXITSTATUS(st) : S_ERR; /* return child status */
  else
    st = S_ERR; /* error */

  mutt_sig_unblock_system(true);

  return st;
}

/**
 * add_args_one - Add an Address to a dynamic array
 * @param[in, out] args    Array to add to
 * @param[in]  addr    Address to add
 */
static void add_args_one(struct SendmailArgs *args, const struct Address *addr)
{
  /* weed out group mailboxes, since those are for display only */
  if (addr->mailbox && !addr->group)
  {
    ARRAY_ADD(args, addr->mailbox);
  }
}

/**
 * add_args - Add a list of Addresses to a dynamic array
 * @param[in, out] args    Array to add to
 * @param[in]  al      Addresses to add
 */
static void add_args(struct SendmailArgs *args, struct AddressList *al)
{
  if (!al)
    return;

  struct Address *a = NULL;
  TAILQ_FOREACH(a, al, entries)
  {
    add_args_one(args, a);
  }
}

/**
 * mutt_invoke_sendmail - Run sendmail
 * @param from     The sender
 * @param to       Recipients
 * @param cc       Recipients
 * @param bcc      Recipients
 * @param msg      File containing message
 * @param eightbit Message contains 8bit chars
 * @param sub      Config Subset
 * @retval  0 Success
 * @retval -1 Failure
 */
int mutt_invoke_sendmail(struct AddressList *from, struct AddressList *to,
                         struct AddressList *cc, struct AddressList *bcc,
                         const char *msg, bool eightbit, struct ConfigSubset *sub)
{
  char *ps = NULL, *path = NULL, *s = NULL, *childout = NULL;
  struct SendmailArgs args = ARRAY_HEAD_INITIALIZER;
  struct SendmailArgs extra_args = ARRAY_HEAD_INITIALIZER;
  int i;

#ifdef USE_NNTP
  if (OptNewsSend)
  {
    char cmd[1024];

    const char *c_inews = cs_subset_string(sub, "inews");
    mutt_expando_format(cmd, sizeof(cmd), 0, sizeof(cmd), NONULL(c_inews),
                        nntp_format_str, 0, MUTT_FORMAT_NO_FLAGS);
    if (*cmd == '\0')
    {
      i = nntp_post(Context->mailbox, msg);
      unlink(msg);
      return i;
    }

    s = mutt_str_dup(cmd);
  }
  else
#endif
  {
    const char *c_sendmail = cs_subset_string(sub, "sendmail");
    s = mutt_str_dup(c_sendmail);
  }

  /* ensure that $sendmail is set to avoid a crash. http://dev.mutt.org/trac/ticket/3548 */
  if (!s)
  {
    mutt_error(_("$sendmail must be set in order to send mail"));
    return -1;
  }

  ps = s;
  i = 0;
  while ((ps = strtok(ps, " ")))
  {
    if (i)
    {
      if (mutt_str_equal(ps, "--"))
        break;
      ARRAY_ADD(&args, ps);
    }
    else
    {
      path = mutt_str_dup(ps);
      ps = strrchr(ps, '/');
      if (ps)
        ps++;
      else
        ps = path;
      ARRAY_ADD(&args, ps);
    }
    ps = NULL;
    i++;
  }

#ifdef USE_NNTP
  if (!OptNewsSend)
  {
#endif
    /* If $sendmail contained a "--", we save the recipients to append to
     * args after other possible options added below. */
    if (ps)
    {
      ps = NULL;
      while ((ps = strtok(ps, " ")))
      {
        ARRAY_ADD(&extra_args, ps);
      }
    }

    const bool c_use_8bitmime = cs_subset_bool(sub, "use_8bitmime");
    if (eightbit && c_use_8bitmime)
      ARRAY_ADD(&args, "-B8BITMIME");

    const bool c_use_envelope_from = cs_subset_bool(sub, "use_envelope_from");
    if (c_use_envelope_from)
    {
      const struct Address *c_envelope_from_address =
          cs_subset_address(sub, "envelope_from_address");
      if (c_envelope_from_address)
      {
        ARRAY_ADD(&args, "-f");
        add_args_one(&args, c_envelope_from_address);
      }
      else if (!TAILQ_EMPTY(from) && !TAILQ_NEXT(TAILQ_FIRST(from), entries))
      {
        ARRAY_ADD(&args, "-f");
        add_args(&args, from);
      }
    }

    const char *c_dsn_notify = cs_subset_string(sub, "dsn_notify");
    if (c_dsn_notify)
    {
      ARRAY_ADD(&args, "-N");
      ARRAY_ADD(&args, c_dsn_notify);
    }

    const char *c_dsn_return = cs_subset_string(sub, "dsn_return");
    if (c_dsn_return)
    {
      ARRAY_ADD(&args, "-R");
      ARRAY_ADD(&args, c_dsn_return);
    }
    ARRAY_ADD(&args, "--");
    const char **e = NULL;
    ARRAY_FOREACH(e, &extra_args)
    {
      ARRAY_ADD(&args, *e);
    }
    add_args(&args, to);
    add_args(&args, cc);
    add_args(&args, bcc);
#ifdef USE_NNTP
  }
#endif

  ARRAY_ADD(&args, NULL);

  /* Some user's $sendmail command uses gpg for password decryption,
   * and is set up to prompt using ncurses pinentry.  If we
   * mutt_endwin() it leaves other users staring at a blank screen.
   * So instead, just force a hard redraw on the next refresh. */
  if (!OptNoCurses)
    mutt_need_hard_redraw();

  const short c_sendmail_wait = cs_subset_number(sub, "sendmail_wait");
  i = send_msg(path, &args, msg, OptNoCurses ? NULL : &childout, c_sendmail_wait);
  if (i != (EX_OK & 0xff))
  {
    if (i != S_BKG)
    {
      const char *e = mutt_str_sysexit(i);
      mutt_error(_("Error sending message, child exited %d (%s)"), i, NONULL(e));
      if (childout)
      {
        struct stat st;

        if ((stat(childout, &st) == 0) && (st.st_size > 0))
        {
          mutt_do_pager(_("Output of the delivery process"), childout,
                        MUTT_PAGER_NO_FLAGS, NULL);
        }
      }
    }
  }
  else if (childout)
    unlink(childout);

  FREE(&childout);
  FREE(&path);
  FREE(&s);
  ARRAY_FREE(&args);
  ARRAY_FREE(&extra_args);

  if (i == (EX_OK & 0xff))
    i = 0;
  else if (i == S_BKG)
    i = 1;
  else
    i = -1;
  return i;
}