#pragma once

#include "utils.hpp"

// Ctrl+C во время передачи
static volatile sig_atomic_t g_cancel = 0;

static void onSigint(int)
{
  g_cancel = 1;
}

// запуск ssh/scp/rsync
static void execArgs(const std::vector<std::string> &args)
{
  std::vector<char *> argv;

  for (const auto &a : args)
  {
    argv.push_back(const_cast<char *>(a.c_str()));
  }

  argv.push_back(nullptr);
  execvp(argv[0], argv.data());
}

static int runCapture(const std::vector<std::string> &args, std::string *out)
{
  int fd[2];

  if (pipe(fd) != 0)
  {
    return -1;
  }

  pid_t pid = fork();

  if (pid < 0)
  {
    close(fd[0]);
    close(fd[1]);
    return -1;
  }

  if (pid == 0)
  {
    int devnull = open("/dev/null", O_RDWR);
    dup2(devnull, 0);
    dup2(fd[1], 1);
    dup2(devnull, 2);
    close(fd[0]);
    close(fd[1]);
    execArgs(args);
    _exit(127);
  }

  close(fd[1]);
  char buf[65536];

  for (;;)
  {
    ssize_t n = read(fd[0], buf, sizeof(buf));

    if (n > 0)
    {
      if (out)
      {
        out->append(buf, n);
      }
    }

    else if (n < 0 && errno == EINTR)
    {
      continue;
    }

    else
    {
      break;
    }
  }

  close(fd[0]);
  int status = 0;

  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
  {
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// то же но вход и выход берутся из файлов (inFd/outFd, -1 = /dev/null) а ошибки кидаем в err
// т.о. скачиваем и заливаем файл через "ssh cat" без scp
static int runPiped(const std::vector<std::string> &args, int inFd, int outFd, std::string *err)
{
  int ep[2];

  if (pipe(ep) != 0)
  {
    return -1;
  }

  pid_t pid = fork();

  if (pid < 0)
  {
    close(ep[0]);
    close(ep[1]);
    return -1;
  }

  if (pid == 0)
  {
    int devnull = open("/dev/null", O_RDWR);
    dup2(inFd >= 0 ? inFd : devnull, 0);
    dup2(outFd >= 0 ? outFd : devnull, 1);
    dup2(ep[1], 2);
    close(ep[0]);
    close(ep[1]);
    execArgs(args);
    _exit(127);
  }

  close(ep[1]);
  char buf[4096];

  for (;;)
  {
    ssize_t n = read(ep[0], buf, sizeof(buf));

    if (n > 0)
    {
      if (err)
      {
        err->append(buf, n);
      }
    }

    else if (n < 0 && errno == EINTR)
    {
      continue;
    }

    else
    {
      break;
    }
  }

  close(ep[0]);
  int status = 0;

  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
  {
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// очистка экрана после завершения
static void clearIfNoAltScreen()
{
  const char *smcup = tigetstr("smcup");

  if (smcup != nullptr && smcup != (char *)-1 && *smcup)
  {
    return;
  }

  const char *cl = tigetstr("clear");

  if (cl != nullptr && cl != (char *)-1)
  {
    putp(cl);
  }

  fflush(stdout);
}

static void leaveCurses()
{
  endwin();
  clearIfNoAltScreen();
}

// выход из интерфейса для ввода пароля и тд
static int runInTerminal(const std::vector<std::string> &args, const std::string &title)
{
  def_prog_mode();
  leaveCurses();
  printf("\n%s\n", title.c_str());
  fflush(stdout);
  // Ctrl+C во время передачи отменяет ТОЛЬКО передачу
  void (*oldInt)(int) = signal(SIGINT, SIG_IGN);
  pid_t pid = fork();

  if (pid == 0)
  {
    signal(SIGINT, SIG_DFL);
    execArgs(args);
    perror(args[0].c_str());
    _exit(127);
  }

  int status = 0;

  if (pid > 0)
  {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
  }

  signal(SIGINT, oldInt);
  int rc = (pid < 0) ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);

  if (rc != 0)
  {
    printf("\nFailed. Press Enter to go back...");
    fflush(stdout);
    int c;

    while ((c = getchar()) != '\n' && c != EOF)
    {
    }
  }

  reset_prog_mode();
  clearok(stdscr, TRUE);
  refresh();
  return rc;
}
