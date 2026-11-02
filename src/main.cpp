#include "app.hpp"

int main(int argc, char *argv[])
{
  if (argc > 3)
  {
    printf("Usage: %s [user@host[:path] [sftp|ssh]]\n", argv[0]);
    return 1;
  }

  Protocol protocol = Protocol::Sftp;

  if (argc == 3)
  {
    std::string mode = argv[2];

    if (mode == "ssh")
    {
      protocol = Protocol::Ssh;
    }

    else if (mode != "sftp")
    {
      fprintf(stderr, "Unknown protocol: %s (expected sftp or ssh)\n", argv[2]);
      return 1;
    }
  }

  setlocale(LC_ALL, ""); // для названий на русском
  std::string local = absolutePath(".");

  if (local.empty())
  {
    perror("realpath");
    return 1;
  }

  const char *home = getenv("HOME");

  if (home)
  {
    mkdir((std::string(home) + "/.ssh").c_str(), 0700); // файл подключения
  }

  loadConfig();
  initscr();
  // raw а не cbreak тк Ctrl+C идет как обычная клавиша
  // закрываем подключение возвращаем терминал а не убиваем
  raw();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  set_escdelay(25); // чтобы Esc срабатывал сразу

  if (argc < 3 && !pickProtocol(protocol))
  {
    leaveCurses();
    return 0;
  }

  std::string target = (argc >= 2) ? argv[1] : pickServer();

  if (target.empty())
  {
    leaveCurses();
    return 0;
  }

  App app(parseTarget(target), local, protocol);
  // сервер не из ~/.ssh/config
  // после подключения спрашиваем сохранить ли данные подключения
  std::vector<std::string> known = sshConfigHosts();
  std::string hostOnly = app.hostName();
  app.offerSave = std::find(known.begin(), known.end(), hostOnly) == known.end() &&
                  std::find(known.begin(), known.end(), parseTarget(target).ssh_host) == known.end();
  int rc = app.run();
  leaveCurses();

  if (!app.fatal.empty())
  {
    fprintf(stderr, "%s\n", app.fatal.c_str());
  }

  return rc;
}
