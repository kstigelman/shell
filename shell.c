/* 
 * Filename   : tsh.c
 * Author     : Kyler Stigelman
 * Assignment : Shell Core
 * Description: tsh - A tiny shell program
 *    
 */

/*
 *******************************************************************************
 * INCLUDE DIRECTIVES
 *******************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 *******************************************************************************
 * TYPE DEFINITIONS
 *******************************************************************************
 */

typedef void handler_t (int);

/*
 *******************************************************************************
 * PREPROCESSOR DEFINITIONS
 *******************************************************************************
 */

// max line size 
#define MAXLINE 1024 
// max args on a command line 
#define MAXARGS 128

/*
 *******************************************************************************
 * GLOBAL VARIABLES
 *******************************************************************************
 */

// defined in libc
extern char** environ;   

// command line prompt 
char prompt[] = "tsh> ";

// for composing sprintf messages
char sbuf[MAXLINE];

// PID of the foreground job's leader, or 0 if there is no foreground job
volatile pid_t g_runningPid = 0;
// PID of the suspended job's leader, or 0 if there is no suspended job
volatile pid_t g_suspendedPid = 0; 

/*
 *******************************************************************************
 * FUNCTION PROTOTYPES
 *******************************************************************************
 */
int
parseline (const char* cmdline, char**argv);

void
sigint_handler (int sig);

void
sigtstp_handler (int sig);

void
sigchld_handler (int sig);

void
sigquit_handler (int sig);

void
unix_error (char* msg);

void
app_error (char* msg);

handler_t*
Signal (int signum, handler_t* handler);

void
eval (const char* cmdline);

void
waitfg ();

int
builtin_cmd (char** arg);

void
makefg ();
/*
 *******************************************************************************
 * MAIN
 *******************************************************************************
 */

int
main (int argc, char** argv)
{
  /* Redirect stderr to stdout */
  dup2 (1, 2);

  /* Install signal handlers */
  Signal (SIGINT, sigint_handler);   /* ctrl-c */
  Signal (SIGTSTP, sigtstp_handler); /* ctrl-z */
  Signal (SIGCHLD, sigchld_handler); /* Terminated or stopped child */
  Signal (SIGQUIT, sigquit_handler); /* quit */

  while (1) {
    printf (prompt);
    fgets (sbuf, MAXLINE, stdin);
    fflush (stdout);
    if (feof (stdin))
      exit (0);
    eval (sbuf);
  }
  /* Quit */
  exit (0);
}

/*
 *  parseline - Parse the command line and build the argv array.
 *
 *  Characters enclosed in single quotes are treated as a single
 *  argument.
 *
 *  Returns true if the user has requested a BG job, false if
 *  the user has requested a FG job.
 */
int
parseline (const char* cmdline, char** argv)
{
  static char array[MAXLINE]; /* holds local copy of command line*/
  char* buf = array;          /* ptr that traverses command line*/
  char* delim;                /* points to first space delimiter*/
  int argc;                   /* number of args*/
  int bg;                     /* background job?*/

  strcpy (buf, cmdline);
  buf[strlen (buf) - 1] = ' ';  /* replace trailing '\n' with space*/
  while (*buf && (*buf == ' ')) /* ignore leading spaces*/
    buf++;


  /* Build the argv list*/
  argc = 0;
  if (*buf == '\'')
  {
    buf++;
    delim = strchr (buf, '\'');
  }
  else
  {
    delim = strchr (buf, ' ');
  }

  while (delim)
  {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces*/
      buf++;

    if (*buf == '\'')
    {
      buf++;
      delim = strchr (buf, '\'');
    }
    else
    {
      delim = strchr (buf, ' ');
    }
  }
  argv[argc] = NULL;

  if (argc == 0) /* ignore blank line*/
    return 1;

  /* should the job run in the background?*/
  if ((bg = (*argv[argc - 1] == '&')) != 0)
  {
    argv[--argc] = NULL;
  }
  return bg;
}
/*
 *  eval - Forks process and executes the user's command in the child 
 *     process.  If a job is in the background, print the command and
 *     child pid. Otherwise, sleep for 2 seconds then wait for foreground
 *     process to be reaped.
 */
void 
eval (const char* cmdline)
{
  char* argv[MAXARGS];
  char buf[MAXLINE];

  strcpy (buf, cmdline);
  int bg = parseline (buf, argv);

  if (argv[0] == NULL)
    return;

  //Check for built-in commands
  if (builtin_cmd (argv)) 
    return;
  
  sigset_t mask, prev;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);
  sigaddset (&mask, SIGINT);
  sigaddset (&mask, SIGTSTP);
  sigprocmask (SIG_BLOCK, &mask, &prev);

  pid_t pid = fork (); 

  if (pid < 0)
  {
    fprintf (stderr, "fork error (%s) -- exiting\n",
            strerror (errno));
    exit (1);
  }
  if (pid == 0)
  { 
    setpgid (0, 0);
    sigprocmask (SIG_SETMASK, &prev, NULL);

    execvp (argv[0], argv);
    printf ("%s: Command not found\n", strtok(buf, "\n"));
    exit (1);
  }
  g_runningPid = pid;
  
  if (bg) {
    printf ("(%d) %s", pid, cmdline);
    g_runningPid = 0;
  }
  
  waitfg ();
  sigprocmask (SIG_SETMASK, &prev, NULL);
}
/*
 *******************************************************************************
 * SIGNAL HANDLERS
 *******************************************************************************
 */

/*
 *  sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *      a child job terminates (becomes a zombie), or stops because it
 *      received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *      available zombie children, but doesn't wait for any other
 *      currently running children to terminate.
 */
void
sigchld_handler (int sig)
{
  pid_t pid;
  int status;

  while ((pid = waitpid (-1, &status, WNOHANG)) > 0) {
    if (g_runningPid == pid) {
      if (WIFSIGNALED (status) && WTERMSIG (status) == SIGINT)
          printf ("Job (%d) terminated by signal %d\n", pid, SIGINT);
      g_runningPid = 0;
    }
  } 
}

/*
 *  sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *     user types ctrl-c at the keyboard.  Catch it and send it along
 *     to the foreground job.
 */
void
sigint_handler (int sig)
{
  if (!g_runningPid) return;
  kill (-g_runningPid, SIGINT);
}

/*
 *  sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *      the user types ctrl-z at the keyboard. Catch it and suspend the
 *      foreground job by sending it a SIGTSTP.
 */
void
sigtstp_handler (int sig)
{
  if (!g_runningPid) return;
  printf ("Job (%d) stopped by signal %d\n", g_runningPid, SIGTSTP);
  kill (-g_runningPid, SIGTSTP);
  makefg ();
}

/*
 *******************************************************************************
 * HELPER ROUTINES
 *******************************************************************************
 */

/*
 * unix_error - unix-style error routine
 */
void
unix_error (char* msg)
{
  fprintf (stdout, "%s: %s\n", strtok (msg, "\n"), strerror (errno));
  exit (1);
}

/*
 *  app_error - application-style error routine
 */
void
app_error (char* msg)
{
  fprintf (stdout, "%s\n", msg);
  exit (1);
}

/*
 *  Signal - wrapper for the sigaction function
 */
handler_t*
Signal (int signum, handler_t* handler)
{
  struct sigaction action, old_action;

  action.sa_handler = handler;
  sigemptyset (&action.sa_mask); /* block sigs of type being handled*/
  action.sa_flags = SA_RESTART;  /* restart syscalls if possible*/

  if (sigaction (signum, &action, &old_action) < 0)
    unix_error ("Signal error");
  return (old_action.sa_handler);
}

/*
 *  sigquit_handler - The driver program can gracefully terminate the
 *     child shell by sending it a SIGQUIT signal.
 */
void
sigquit_handler (int sig)
{
  printf ("Terminating after receipt of SIGQUIT signal\n");
  exit (1);
}

/*
 *  waitfg - Waits for the foreground process to
 *     to receive a SIGCHLD
 */
void waitfg () {
  sigset_t prev;
  sigemptyset (&prev);
  while (g_runningPid)
    sigsuspend (&prev);
}

/*
 *  builtin_cmd - Checks if the user specified either "quit" or "fg".
 *     Exit program if the user typed "quit", or if the user typed "fg",
 *     send a SIGCONT to the suspended process group and return value 1 to main.
 */
int builtin_cmd (char** arg) 
{
  if (!strcmp (arg[0], "quit")) {
    exit (0);
  }
  if (!strcmp (arg[0], "fg")) {
    makefg ();
    waitfg ();
    return 1;
  }
  return 0;
}

/*
 *  makefg - Brings the currently suspended process to the foreground,
 *     if one exists. Then, switch the pid of the running process and
 *     the suspended process.
 */
void makefg () {
  if (g_suspendedPid) 
    kill (-g_suspendedPid, SIGCONT);
  pid_t temp = g_runningPid;
  g_runningPid = g_suspendedPid;
  g_suspendedPid = temp;
}

