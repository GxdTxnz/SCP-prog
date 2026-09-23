#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <ftw.h>
#include <glob.h>
#include <grp.h>
#include <iterator>
#include <map>
#include <ncurses.h>
#include <poll.h>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

/*
все что больше - передаем через rsync (нужен на двух серверах)
меняется в ~/.scpanel.config в rsync_from
*/
static const long long RSYNC_FROM_DEFAULT = 100LL * 1024 * 1024; // 100 МБ
static const long long RSYNC_NEVER = -1;                         // rsync_from = never - всегда scp

// клавиши можно переопределить в ~/.scpanel.config в секции [keys]
// name - как действие пишется в конфиге, keys - дефолт клавиши, help - строка помощи
enum Act
{
  ACT_NONE,
  ACT_UP,
  ACT_DOWN,
  ACT_PAGEUP,
  ACT_PAGEDOWN,
  ACT_HOME,
  ACT_END,
  ACT_PARENT,
  ACT_OPEN,
  ACT_PANEL,
  ACT_MARK,
  ACT_MARKALL,
  ACT_SEND,
  ACT_VIEW,
  ACT_EDIT,
  ACT_RENAME,
  ACT_MKDIR,
  ACT_NEWFILE,
  ACT_DELETE,
  ACT_GOTO,
  ACT_SORT,
  ACT_SEARCH,
  ACT_NEXT,
  ACT_PREV,
  ACT_REFRESH,
  ACT_HELP,
  ACT_QUIT,
  ACT_CANCEL, // работает только пока идет передача поэтому может делить действие с другими биндами
  ACT_COUNT
};

struct ActionInfo
{ const char *name, *keys, *help; };

static const ActionInfo ACTIONS[ACT_COUNT] = {
    {"", "", ""},
    {"up", "Up", "move up"},
    {"down", "Down", "move down"},
    {"pageup", "PgUp", "page up"},
    {"pagedown", "PgDn", "page down"},
    {"home", "Home", "first item"},
    {"end", "End", "last item"},
    {"parent", "Left Backspace", "go to parent folder"},
    {"open", "Enter Right", "open folder / send file"},
    {"panel", "Tab", "other panel"},
    {"mark", "Space", "mark"},
    {"markall", "a", "mark all / unmark all"},
    {"send", "s F5", "send to the other panel"},
    {"view", "v F3", "view file"},
    {"edit", "e F4", "edit file"},
    {"rename", "m F6", "rename"},
    {"mkdir", "f F7", "new folder"},
    {"newfile", "c", "new empty file"},
    {"delete", "d F8 Del", "delete"},
    {"goto", "g", "go to path"},
    {"sort", "o", "sort: name / date / size"},
    {"search", "/", "search"},
    {"next", "n", "next match"},
    {"prev", "p", "previous match"},
    {"refresh", "r", "refresh"},
    {"help", "h F1", "this help"},
    {"quit", "q Ctrl+C", "quit"},
    {"cancel", "Esc q Ctrl+C", "cancel transfer (while it runs)"},
};

// недокачанное
static const char *PARTIAL_DIR = ".rsync-partial";

static const char *SORT_NAMES[] = {"name", "date (newest first)", "size (largest first)"};

// Ctrl+C во время передачи
static volatile sig_atomic_t g_cancel = 0;
static void onSigint(int)
{ g_cancel = 1; }

static std::string shq(const std::string &s)
{
  std::string r = "'";

  for (char c : s) r += (c == '\'') ? std::string("'\\''") : std::string(1, c);

  return r + "'";
}

// "/home/user/x" -> "/home/user", "/home" -> "/"
static std::string parentOf(const std::string &path)
{
  size_t pos = path.find_last_of('/');

  if (pos == 0 || pos == std::string::npos)
    return "/";

  return path.substr(0, pos);
}

// чтобы в корне не получалось "//etc"
static std::string joinPath(const std::string &dir, const std::string &name)
{ return dir == "/" ? "/" + name : dir + "/" + name; }

// "/tmp" -> "/tmp/", "/" -> "/"
static std::string dirArg(const std::string &dir)
{ return dir == "/" ? "/" : dir + "/"; }

static std::string absolutePath(const std::string &path)
{
  char buf[PATH_MAX];

  if (realpath(path.c_str(), buf) == nullptr)
    return "";

  return buf;
}

static std::string trim(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\r\n");

  if (a == std::string::npos)
    return "";

  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// cтираем последний символ даже если это русская буква из двух байт
static void popUtf8(std::string &s)
{
  while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();

  if (!s.empty())
    s.pop_back();
}

// 1536 -> "1.5K", 34000000 -> "32M"
static std::string humanSize(long long b)
{
  const char units[] = "BKMGTP";
  double v = (double)b;
  int i = 0;

  while (v >= 1024 && i < 5)
  {
    v /= 1024;
    ++i;
  }

  char buf[32];

  if (i == 0)
    snprintf(buf, sizeof(buf), "%lld", b);

  else
    snprintf(buf, sizeof(buf), v < 10 ? "%.1f%c" : "%.0f%c", v, units[i]);

  return buf;
}

// то же но с "B" для небольших размеров
static std::string sizeText(long long b)
{ return humanSize(b) + (b < 1024 ? "B" : ""); }

// 1695481234 -> "2023-09-23 18:00"
static std::string formatDate(long long t)
{
  if (t <= 0)
    return "";

  time_t tt = (time_t)t;
  struct tm tm{};
  localtime_r(&tt, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

// 75 -> "1:15", 3725 -> "1:02:05"
static std::string formatDuration(double sec)
{
  long long s = (long long)(sec + 0.5);
  char buf[32];

  if (s >= 3600)
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);

  else
    snprintf(buf, sizeof(buf), "%lld:%02lld", s / 60, s % 60);

  return buf;
}

// секунды с какого то момента
// чтобы мерить промежутки
static double nowSec()
{
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// первая непустая строка
// в выводе scp/rsync/rm обычно сама ошибка
static std::string firstLine(const std::string &s)
{
  std::istringstream iss(s);
  std::string line;

  while (std::getline(iss, line))
  {
    line = trim(line);

    if (!line.empty())
      return line;
  }

  return "";
}

static bool readFile(const std::string &path, std::string &out)
{
  std::ifstream f(path, std::ios::binary);

  if (!f)
    return false;

  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// проверка наличия в PATH
static bool inPath(const std::string &prog)
{
  const char *env = getenv("PATH");
  if (env == nullptr)
    return false;

  std::stringstream ss(env);
  std::string dir;

  while (std::getline(ss, dir, ':'))
  {
    if (dir.empty())
      dir = ".";

    if (access((dir + "/" + prog).c_str(), X_OK) == 0)
      return true;
  }

  return false;
}

/*
русская буква - 2 байта но 1 клетка на экране
иероглиф - 2 клетки
обрезаем строки через подсчет клеток

проход по символам: f(с какого байта, сколько байт, сколько клеток, можно ли вывести)
*/
template <class F> static void forEachChar(const std::string &s, F f)
{
  mbstate_t st{};
  size_t i = 0;

  while (i < s.size())
  {
    wchar_t wc = 0;
    size_t n = mbrtowc(&wc, s.data() + i, s.size() - i, &st);
    bool bad = (n == (size_t)-1 || n == (size_t)-2);

    if (bad)
    {
      n = 1;
      st = mbstate_t();
    }

    else if (n == 0)
      n = 1;

    int w = bad ? -1 : wcwidth(wc);
    bool printable = !bad && w >= 0 && !iswcntrl(wc);

    if (!f(i, n, printable ? w : 1, printable))
      return;

    i += n;
  }
}

static int colsOf(const std::string &s)
{
  int total = 0;
  forEachChar(s, [&](size_t, size_t, int w, bool) {
    total += w;
    return true;
  });
  return total;
}

// подгон строки под кол-во клеток; длинную обрезаем с добавлением "…"; короткую добиваем пробелами
// символы которые невозможно вывести заменяем на "?"
static std::string fitCols(const std::string &s, int w)
{
  if (w <= 0)
    return "";

  bool cut = colsOf(s) > w;
  int limit = cut ? w - 1 : w;
  std::string out;
  int used = 0;

  forEachChar(s, [&](size_t i, size_t n, int cw, bool printable) {
    if (used + cw > limit)
      return false;

    if (printable)
      out.append(s, i, n);

    else
      out += '?';

    used += cw;
    return true;
  });

  if (cut)
  {
    out += "…";
    ++used;
  }

  if (used < w)
    out.append(w - used, ' ');

  return out;
}

// поиск без учета регистра
static std::wstring lowerW(const std::string &s)
{
  std::wstring out;
  mbstate_t st{};
  size_t i = 0;

  while (i < s.size())
  {
    wchar_t wc = 0;
    size_t n = mbrtowc(&wc, s.data() + i, s.size() - i, &st);

    if (n == (size_t)-1 || n == (size_t)-2)
    {
      wc = (unsigned char)s[i];
      n = 1;
      st = mbstate_t();
    }

    else if (n == 0)
      n = 1;

    out += (wchar_t)towlower(wc);
    i += n;
  }

  return out;
}

// запуск ssh/scp/rsync
static void execArgs(const std::vector<std::string> &args)
{
  std::vector<char *> argv;

  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));

  argv.push_back(nullptr);
  execvp(argv[0], argv.data());
}

static int runCapture(const std::vector<std::string> &args, std::string *out)
{
  int fd[2];

  if (pipe(fd) != 0)
    return -1;

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
        out->append(buf, n);
    }

    else if (n < 0 && errno == EINTR)
      continue;

    else
      break;
  }

  close(fd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// то же но вход и выход берутся из файлов (inFd/outFd, -1 = /dev/null) а ошибки собираем в err
// таким образом скачиваем и заливаем файл через "ssh cat" без scp
static int runPiped(const std::vector<std::string> &args, int inFd, int outFd, std::string *err)
{
  int ep[2];

  if (pipe(ep) != 0)
    return -1;

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
        err->append(buf, n);
    }

    else if (n < 0 && errno == EINTR)
      continue;

    else
      break;
  }

  close(ep[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// очистка экрана после завершения
static void clearIfNoAltScreen()
{
  const char *smcup = tigetstr("smcup");

  if (smcup != nullptr && smcup != (char *)-1 && *smcup)
    return;

  const char *cl = tigetstr("clear");

  if (cl != nullptr && cl != (char *)-1)
    putp(cl);

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
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

  signal(SIGINT, oldInt);
  int rc = (pid < 0) ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);

  if (rc != 0)
  {
    printf("\nFailed. Press Enter to go back...");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
  }

  reset_prog_mode();
  clearok(stdscr, TRUE);
  refresh();
  return rc;
}

struct Entry
{
  std::string name, owner, group;
  char type = '?';     // d директория, l ссылка, f файл, ? хз
  bool is_dir = false; // директория или ссылка на директорию
  unsigned mode = 0;   // права (755 и тд)
  long long size = 0;
  long long mtime = 0; // когда меняли, секунды с 1970
};

static std::string permString(char type, unsigned mode)
{
  if (type == '?')
    return "??????????";

  std::string s(10, '-');
  s[0] = (type == 'f') ? '-' : type;
  const char rwx[] = "rwxrwxrwx";

  for (int i = 0; i < 9; ++i)
    if (mode & (0400u >> i))
      s[i + 1] = rwx[i];

  return s;
}

static char typeOf(mode_t m)
{
  if (S_ISDIR(m))
    return 'd';

  if (S_ISLNK(m))
    return 'l';

  if (S_ISREG(m))
    return 'f';

  if (S_ISFIFO(m))
    return 'p';

  if (S_ISSOCK(m))
    return 's';

  if (S_ISCHR(m))
    return 'c';

  if (S_ISBLK(m))
    return 'b';

  return '?';
}

// Сначала директории потом файлы
// mode: 0 - по алфавиту, 1 - новые сверху, 2 - большие сверху
static void sortEntries(std::vector<Entry> &v, int mode)
{
  std::sort(v.begin(), v.end(), [mode](const Entry &a, const Entry &b) {
    if (a.is_dir != b.is_dir)
      return a.is_dir;

    if (mode == 1 && a.mtime != b.mtime)
      return a.mtime > b.mtime;

    if (mode == 2 && a.size != b.size)
      return a.size > b.size;

    return a.name < b.name;
  });
}

static bool readLocalDir(const std::string &path, std::vector<Entry> &out, std::string &err)
{
  DIR *dir = opendir(path.c_str());

  if (dir == nullptr)
  {
    err = "Cannot open " + path + ": " + strerror(errno);
    return false;
  }
  out.clear();

  while (dirent *d = readdir(dir))
  {
    Entry e;
    e.name = d->d_name;

    if (e.name == "." || e.name == "..")
      continue;

    std::string full = joinPath(path, e.name);
    struct stat lst{};

    if (lstat(full.c_str(), &lst) == 0)
    {
      e.type = typeOf(lst.st_mode);
      e.mode = lst.st_mode & 0777;
      e.size = lst.st_size;
      e.mtime = lst.st_mtime;
      struct stat st{};
      e.is_dir = e.type == 'd' || (e.type == 'l' && stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
      passwd *pw = getpwuid(lst.st_uid);
      group *gr = getgrgid(lst.st_gid);
      e.owner = pw ? pw->pw_name : std::to_string(lst.st_uid);
      e.group = gr ? gr->gr_name : std::to_string(lst.st_gid);
    }

    out.push_back(e);
  }

  closedir(dir);
  return true;
}

// размер файлов/директорий для выбора между scp и rsync
static long long g_sum = 0, g_limit = 0;
static int sumCallback(const char *, const struct stat *sb, int flag, struct FTW *)
{
  if (flag == FTW_F)
    g_sum += sb->st_size;

  return g_sum >= g_limit ? 1 : 0;
}
static long long localSize(const std::vector<std::string> &paths, long long limit)
{
  g_sum = 0;
  g_limit = limit;

  for (const auto &p : paths)
    if (nftw(p.c_str(), sumCallback, 32, FTW_PHYS) == 1)
      break;

  return g_sum;
}

// удаление директории со всем содержимым
static int g_rmErrno = 0;
static int rmCallback(const char *path, const struct stat *, int, struct FTW *)
{
  if (remove(path) == 0)
    return 0;

  g_rmErrno = errno;
  return -1;
}

static bool removeLocal(const std::string &path, std::string &err)
{
  g_rmErrno = 0;

  if (nftw(path.c_str(), rmCallback, 32, FTW_DEPTH | FTW_PHYS) == 0)
    return true;

  err = path + ": " + strerror(g_rmErrno ? g_rmErrno : errno);
  return false;
}

// логика как в mkdir -p
static bool makeDirs(const std::string &path, std::string &err)
{
  for (size_t pos = 1; pos <= path.size(); ++pos)
  {
    if (pos != path.size() && path[pos] != '/')
      continue;

    std::string part = path.substr(0, pos);

    if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
    {
      err = part + ": " + strerror(errno);
      return false;
    }
  }

  return true;
}

struct Target
{
  std::string scp_host; // user@host или user@[::1] - формат scp и rsync
  std::string ssh_host; // то же без квадратных скобок - формат ssh
  std::string path;     // директория на сервере, пусто = домашка
};

static Target parseTarget(const std::string &t)
{
  Target r;
  size_t from = t.rfind(']');
  size_t colon = t.find(':', from == std::string::npos ? 0 : from);
  r.scp_host = colon == std::string::npos ? t : t.substr(0, colon);
  r.path = colon == std::string::npos ? "" : t.substr(colon + 1);

  for (char c : r.scp_host)
    if (c != '[' && c != ']')
      r.ssh_host += c;
  return r;
}

// настройка конфига
static std::string lower(std::string s)
{
  for (char &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

static std::string homeDir()
{
  const char *home = getenv("HOME");
  return home ? home : "";
}

/*
имя клавиши из конфига -> коды которые отдает getch()
"s", "/" - сама буква; F1..F12; Ctrl+A..Ctrl+Z; Up Down Left Right PgUp PgDn Home End;
Enter Tab Space Esc Backspace Del Ins
*/
static std::vector<int> keyCodes(const std::string &name)
{
  if (name.size() == 1 && isgraph((unsigned char)name[0]))
    return {(unsigned char)name[0]};

  std::string n = lower(name);

  if (n.size() == 6 && n.compare(0, 5, "ctrl+") == 0 && isalpha((unsigned char)n[5]))
    return {n[5] - 'a' + 1};

  if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1]))
  {
    int k = atoi(n.c_str() + 1);

    if (k >= 1 && k <= 12 && n == "f" + std::to_string(k))
      return {KEY_F(k)};
  }

  static const std::map<std::string, std::vector<int>> named = {
      {"up", {KEY_UP}},
      {"down", {KEY_DOWN}},
      {"left", {KEY_LEFT}},
      {"right", {KEY_RIGHT}},
      {"pgup", {KEY_PPAGE}},
      {"pgdn", {KEY_NPAGE}},
      {"home", {KEY_HOME}},
      {"end", {KEY_END}},
      {"enter", {'\n', '\r', KEY_ENTER}},
      {"tab", {'\t'}},
      {"space", {' '}},
      {"esc", {27}},
      {"backspace", {KEY_BACKSPACE, 127, 8}}, // разные терминалы шлют разное
      {"del", {KEY_DC}},
      {"ins", {KEY_IC}},
  };
  auto it = named.find(n);
  return it == named.end() ? std::vector<int>() : it->second;
}

struct Config
{
  std::vector<std::string> keys[ACT_COUNT]; // имена клавиш для каждого действия (уже с учетом конфига)
  std::map<int, int> keymap;                // код клавиши -> действие (кроме cancel)
  std::set<int> cancelKeys;                 // чем отменять передачу
  int sort = 0;                             // см. sortEntries
  long long rsyncFrom = RSYNC_FROM_DEFAULT; // с какого размера передавать через rsync
  std::vector<std::string> warnings;        // что в конфиге не поняли - покажем при запуске
};

static Config g_cfg;

static std::string configPath()
{ return homeDir() + "/.scpanel.config"; }

// Читаем ~/.scpanel.config. Нет файла - все по умолчанию.
// Если в [keys] действие указано, его клавиши по умолчанию заменяются целиком;
// пустое значение ("send =") - у действия вообще не будет клавиш.
static void loadConfig()
{
  Config c;
  bool custom[ACT_COUNT] = {};

  for (int a = 1; a < ACT_COUNT; ++a)
  {
    std::istringstream iss(ACTIONS[a].keys);
    for (std::string k; iss >> k;) c.keys[a].push_back(k);
  }

  std::ifstream f(configPath());
  std::string line, section;
  int no = 0;

  while (std::getline(f, line))
  {
    ++no;
    line = trim(line);

    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;

    std::string where = "~/.scpanel.config line " + std::to_string(no) + ": ";

    if (line[0] == '[')
    {
      section = lower(trim(line.substr(1, line.find(']') - 1)));

      if (section != "keys" && section != "options")
        c.warnings.push_back(where + "unknown section [" + section + "]");
      continue;
    }

    size_t eq = line.find('=');

    if (eq == std::string::npos)
    {
      c.warnings.push_back(where + "expected name = value");
      continue;
    }

    std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));

    if (section == "keys")
    {
      int a = 1;

      while (a < ACT_COUNT && name != ACTIONS[a].name) ++a;

      if (a == ACT_COUNT)
      {
        c.warnings.push_back(where + "unknown action '" + name + "'");
        continue;
      }

      custom[a] = true;
      c.keys[a].clear();
      std::istringstream iss(value);

      for (std::string k; iss >> k;)
      {
        if (keyCodes(k).empty())
          c.warnings.push_back(where + "unknown key '" + k + "'");
        else
          c.keys[a].push_back(k);
      }
    }

    else if (section == "options" && name == "sort")
    {
      std::string v = lower(value);
      c.sort = v == "date" ? 1 : v == "size" ? 2 : 0;

      if (v != "name" && v != "date" && v != "size")
        c.warnings.push_back(where + "sort must be name, date or size");
    }

    else if (section == "options" && name == "rsync_from")
    {
      // "100M", "1G", "512K", "0" (всегда rsync), "never" (всегда scp)
      std::string v = lower(value);
      char *end = nullptr;
      double n = strtod(v.c_str(), &end);
      std::string unit = end ? trim(end) : "";
      const std::map<std::string, double> mult = {{"", 1},
                                                  {"b", 1},
                                                  {"k", 1024},
                                                  {"m", 1024.0 * 1024},
                                                  {"g", 1024.0 * 1024 * 1024},
                                                  {"t", 1024.0 * 1024 * 1024 * 1024}};

      if (v == "never")
        c.rsyncFrom = RSYNC_NEVER;

      else if (end == v.c_str() || n < 0 || !mult.count(unit))
        c.warnings.push_back(where + "rsync_from must be a size like 100M, 1G, 0 or never");

      else
        c.rsyncFrom = (long long)(n * mult.at(unit));
    }

    else
      c.warnings.push_back(where + "unknown setting '" + name + "'");
  }

  // Сначала раскладываем клавиши по умолчанию, потом - из конфига: если клавиша из конфига
  // уже занята другим действием, побеждает конфиг
  for (int pass = 0; pass < 2; ++pass)
    for (int a = 1; a < ACT_CANCEL; ++a)
      if (custom[a] == (pass == 1))
        for (const auto &k : c.keys[a])
          for (int code : keyCodes(k)) c.keymap[code] = a;

  // у действий, у которых конфиг забрал клавишу, убираем ее и из подсказок
  for (int a = 1; a < ACT_CANCEL; ++a)
  {
    std::vector<std::string> kept;

    for (const auto &k : c.keys[a])
      if (c.keymap[keyCodes(k)[0]] == a)
        kept.push_back(k);

    c.keys[a].swap(kept);
  }

  for (const auto &k : c.keys[ACT_CANCEL])
    for (int code : keyCodes(k)) c.cancelKeys.insert(code);

  g_cfg = c;
}

static int actionFor(int ch)
{
  auto it = g_cfg.keymap.find(ch);
  return it == g_cfg.keymap.end() ? ACT_NONE : it->second;
}

// "s / F5" - как клавиши действия показать человеку
static std::string keysText(int a)
{
  std::string r;

  for (const auto &k : g_cfg.keys[a]) r += (r.empty() ? "" : " / ") + k;

  return r.empty() ? "(none)" : r;
}

// нижняя строка-подсказка, из того, что реально назначено
static std::string helpLine()
{
  const std::pair<int, const char *> shown[] = {{ACT_HELP, "help"},     {ACT_PANEL, "panel"},   {ACT_MARK, "mark"},
                                                {ACT_SEND, "send"},     {ACT_EDIT, "edit"},     {ACT_VIEW, "view"},
                                                {ACT_DELETE, "delete"}, {ACT_SEARCH, "search"}, {ACT_QUIT, "quit"}};
  std::string r;

  for (const auto &s : shown)
    if (!g_cfg.keys[s.first].empty())
      r += (r.empty() ? "" : "  ") + g_cfg.keys[s.first][0] + " " + s.second;

  return r;
}

// ---------- список серверов: ~/.ssh/config ----------

// Имена машин из ssh config вместе со всем, что подключено через Include (шаблоны со звездой скип).
// Include как у ssh: путь без / в начале считается от ~/.ssh, можно со звездочками
static void readSshConfig(const std::string &path, std::vector<std::string> &hosts, int depth)
{
  if (depth > 8) // защита от файлов, которые подключают друг друга по кругу
    return;

  std::ifstream f(path);
  std::string line;

  while (std::getline(f, line))
  {
    std::replace(line.begin(), line.end(), '=', ' '); // бывает и "Host=name"
    std::istringstream iss(line);
    std::string key, name;
    iss >> key;
    key = lower(key);

    if (key == "include")
    {
      while (iss >> name)
      {
        if (name.compare(0, 2, "~/") == 0)
          name = homeDir() + name.substr(1);

        else if (name[0] != '/')
          name = homeDir() + "/.ssh/" + name;

        glob_t g{};

        if (glob(name.c_str(), 0, nullptr, &g) == 0)
          for (size_t i = 0; i < g.gl_pathc; ++i) readSshConfig(g.gl_pathv[i], hosts, depth + 1);

        globfree(&g);
      }
      continue;
    }

    if (key != "host")
      continue;

    while (iss >> name)
    {
      if (name.find_first_of("*?!") != std::string::npos)
        continue;

      if (std::find(hosts.begin(), hosts.end(), name) == hosts.end())
        hosts.push_back(name);
    }
  }
}

static std::vector<std::string> sshConfigHosts()
{
  std::vector<std::string> hosts;

  if (!homeDir().empty())
    readSshConfig(homeDir() + "/.ssh/config", hosts, 0);

  return hosts;
}

// Дописать сервер в ~/.ssh/config, чтобы он появился в списке (и для обычного ssh тоже):
//   Host alias
//       HostName host
//       User user
static bool saveSshHost(const std::string &alias, const std::string &user, const std::string &host, std::string &err)
{
  std::string dir = homeDir() + "/.ssh", path = dir + "/config", old;
  mkdir(dir.c_str(), 0700);
  bool had = readFile(path, old);
  // новый файл - сразу с правами 600, как требует ssh
  int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);

  if (fd < 0)
  {
    err = path + ": " + strerror(errno);
    return false;
  }

  std::string text = (had && !old.empty() && old.back() != '\n') ? "\n" : "";
  text += (had && !old.empty() ? "\n" : "") + std::string("Host ") + alias + "\n    HostName " + host + "\n";

  if (!user.empty())
    text += "    User " + user + "\n";

  bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();

  if (!ok)
    err = path + ": " + strerror(errno);

  close(fd);
  return ok;
}

// ввод строки поиска внизу экрана
// Esc - отмена
// initial - что уже вписано в поле (старое имя при переименовании и тд)
static std::string inputLine(const std::string &prompt, const std::string &initial = "")
{
  std::string text = initial;
  curs_set(1);

  while (true)
  {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // длинный текст не влезает - показываем его хвост, там курсор
    std::string shown = text;
    int room = cols - 2 - colsOf(prompt);

    while (room > 1 && colsOf(shown) > room - 1)
    {
      size_t skip = 1;

      while (skip < shown.size() && ((unsigned char)shown[skip] & 0xC0) == 0x80) ++skip;

      shown.erase(0, skip);
    }

    if (shown.size() != text.size())
      shown = "…" + shown;

    move(rows - 1, 0);
    clrtoeol();
    mvaddstr(rows - 1, 0, (prompt + shown).c_str());
    refresh();
    int ch = getch();

    if (ch == 27 || ch == 3) // Esc или Ctrl+C
    {
      text.clear();
      break;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
      break;

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
      popUtf8(text);

    else if (ch >= 32 && ch < 256)
      text += (char)ch;
  }
  curs_set(0);
  return trim(text);
}

// список машин для подключения если запустились без адреса
static std::string pickServer()
{
  std::vector<std::string> items = sshConfigHosts();
  const std::string manual = "+ enter address manually";

  if (items.empty())
    return inputLine("Server address (user@host or user@host:/dir): ");

  items.push_back(manual);
  int hl = 0, top = 0;

  while (true)
  {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int listH = std::max(1, rows - 3);

    if (hl < top)
      top = hl;

    if (hl >= top + listH)
      top = hl - listH + 1;

    erase();
    attron(A_BOLD);
    mvaddstr(0, 0, fitCols("Where to connect? (from ~/.ssh/config)", cols).c_str());
    attroff(A_BOLD);

    for (int r = 0; r < listH && top + r < (int)items.size(); ++r)
    {
      int i = top + r;

      if (i == hl)
        attron(A_REVERSE);

      mvaddstr(r + 2, 0, fitCols("  " + items[i], cols).c_str());

      if (i == hl)
        attroff(A_REVERSE);
    }

    mvaddstr(rows - 1, 0, fitCols("↑↓ select  Enter connect  q quit", cols - 1).c_str());
    refresh();
    int ch = getch();
    int last = (int)items.size() - 1;

    if (ch == KEY_UP && hl > 0)
      --hl;

    else if (ch == KEY_DOWN && hl < last)
      ++hl;

    else if (ch == 'q' || ch == 27 || ch == 3) // q, Esc, Ctrl+C
      return "";

    else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == KEY_RIGHT)
    {
      if (hl != last)
        return items[hl];

      std::string addr = inputLine("Server address (user@host or user@host:/dir): ");

      if (!addr.empty())
        return addr;
    }
  }
}

struct Pane
{
  bool remote = false;
  std::string cwd;
  std::vector<Entry> files;
  std::set<std::string> marked; // отмеченные имена в текущей директории
  int highlight = 0;
  int top = 0; // первая видимая строка
};

class App
{
public:
  std::string fatal;      // вывод ошибки (после закрытия интерфейса)
  bool offerSave = false; // сервера нет в ~/.ssh/config - после подключения предлагаем сохранить

  App(const Target &t, const std::string &localStart) : tgt(t), localStart(localStart)
  {
    panes[0].remote = false;
    panes[1].remote = true;
    std::string id = std::to_string(getpid());
    const char *home = getenv("HOME");
    ctlOpt = "ControlPath=~/.ssh/scpanel-" + id + ".sock";
    ctlFile = std::string(home ? home : "") + "/.ssh/scpanel-" + id + ".sock";
  }

  // "user@host" -> "host"
  std::string hostName() const
  {
    size_t at = tgt.ssh_host.find('@');
    return at == std::string::npos ? tgt.ssh_host : tgt.ssh_host.substr(at + 1);
  }

  int run()
  {
    if (!ensureConnection())
    {
      fatal = "Could not connect to " + tgt.ssh_host;
      return 1;
    }

    rsyncLocal = inPath("rsync");
    std::string out;
    rsyncRemote = remoteSh("command -v rsync >/dev/null 2>&1 && echo yes", out) == 0 && out.compare(0, 3, "yes") == 0;

    if (!loadPane(panes[0], localStart, ""))
    {
      fatal = status;
      closeConnection();
      return 1;
    }
    status.clear();

    if (!loadPane(panes[1], tgt.path, ""))
    {
      std::string why = status;

      if (tgt.path.empty() || !loadPane(panes[1], "", ""))
      {
        fatal = why;
        closeConnection();
        return 1;
      }

      status = why + " - opened home directory";
    }

    if (status.empty() && (!rsyncLocal || !rsyncRemote))
      status = std::string("rsync not found ") + (rsyncLocal ? "on the server" : "on this machine") +
               " - using scp for everything";

    // уведомление о кривом конфиге
    if (!g_cfg.warnings.empty())
      status = g_cfg.warnings[0] +
               (g_cfg.warnings.size() > 1 ? " (and " + std::to_string(g_cfg.warnings.size() - 1) + " more)" : "");

    if (offerSave)
      offerSaveHost();

    while (true)
    {
      draw();
      int ch = getch();

      if (searching)
      {
        searchKey(ch);
        continue;
      }

      Pane &p = panes[active];
      status.clear();
      int last = (int)p.files.size() - 1;
      int listH = std::max(1, getmaxy(stdscr) - 3);

      switch (actionFor(ch))
      {
      case ACT_PANEL:
        active = 1 - active;
        break;

      case ACT_UP:
        if (p.highlight > 0)
          --p.highlight;
        break;

      case ACT_DOWN:
        if (p.highlight < last)
          ++p.highlight;
        break;

      case ACT_PAGEUP:
        p.highlight = std::max(0, p.highlight - listH);
        break;

      case ACT_PAGEDOWN:
        p.highlight = std::max(0, std::min(last, p.highlight + listH));
        break;

      case ACT_HOME:
        p.highlight = 0;
        break;

      case ACT_END:
        p.highlight = std::max(0, last);
        break;

      case ACT_PARENT:
        if (p.cwd != "/") // встаем на папку, из которой вышли
          loadPane(p, parentOf(p.cwd), p.cwd.substr(p.cwd.find_last_of('/') + 1));
        break;

      case ACT_OPEN:
        if (p.files.empty())
          break;

        if (p.files[p.highlight].is_dir)
          loadPane(p, joinPath(p.cwd, p.files[p.highlight].name), "");

        else
          transfer(true);
        break;

      case ACT_MARK:
        if (p.files.empty())
          break;
        {
          const std::string &name = p.files[p.highlight].name;

          if (!p.marked.erase(name))
            p.marked.insert(name);
        }

        if (p.highlight < last)
          ++p.highlight;
        break;

      case ACT_MARKALL:
        if (p.marked.size() == p.files.size())
          p.marked.clear();

        else
          for (const auto &e : p.files) p.marked.insert(e.name);
        break;

      case ACT_SEND:
        if (!p.files.empty())
          transfer(false);
        break;

      case ACT_VIEW:
        viewCurrent();
        break;

      case ACT_EDIT:
        editCurrent();
        break;

      case ACT_RENAME:
        renameCurrent();
        break;

      case ACT_MKDIR:
        makeDir();
        break;

      case ACT_NEWFILE:
        makeFile();
        break;

      case ACT_DELETE:
        deleteSelected();
        break;

      case ACT_GOTO:
        gotoPath();
        break;

      case ACT_SORT:
        cycleSort();
        break;

      case ACT_HELP:
        showHelp();
        break;

      case ACT_SEARCH:
        searching = true;
        search.clear();
        searchFrom = p.highlight;
        searchMiss = false;
        break;

      case ACT_NEXT:
        if (lastSearch.empty())
          status = "Search for something with / first";

        else if (!findMatch(p, lastSearch, p.highlight + 1))
          status = "Not found: " + lastSearch;

        break;

      case ACT_PREV:
        if (lastSearch.empty())
          status = "Search for something with / first";

        else if (!findMatch(p, lastSearch, p.highlight - 1, -1))
          status = "Not found: " + lastSearch;

        break;

      case ACT_REFRESH:
        reloadPane(panes[0]);
        reloadPane(panes[1]);
        break;

      case ACT_QUIT:
        closeConnection();
        return 0;

      default:
        break;
      }
    }
  }

private:
  // если адреса нет в ~/.ssh/config то предлагаем сохранить
  void offerSaveHost()
  {
    size_t at = tgt.ssh_host.find('@');
    std::string user = at == std::string::npos ? "" : tgt.ssh_host.substr(0, at);
    std::string host = at == std::string::npos ? tgt.ssh_host : tgt.ssh_host.substr(at + 1);
    std::string keep = status;

    if (ask("Save " + tgt.ssh_host + " to ~/.ssh/config so it shows up in the list?  y yes  n no", "yn") != 'y')
    {
      status = keep;
      return;
    }

    std::string alias = inputLine("Name in the list: ", host), err;

    if (alias.empty())
      status = keep;

    else if (alias.find_first_of(" \t*?!") != std::string::npos)
      status = "Not saved: the name cannot contain spaces or * ? !";

    else
    {
      std::vector<std::string> hosts = sshConfigHosts();

      if (std::find(hosts.begin(), hosts.end(), alias) != hosts.end())
        status = "Not saved: " + alias + " is already in ~/.ssh/config";

      else if (!saveSshHost(alias, user, host, err))
        status = "Not saved: " + err;

      else
        status = "Saved to ~/.ssh/config as " + alias;
    }
  }

  Target tgt;
  std::string localStart, ctlOpt, ctlFile;
  Pane panes[2];
  int active = 0;
  std::string status;
  bool rsyncLocal = false, rsyncRemote = false;
  bool searching = false, searchMiss = false;
  std::string search, lastSearch;
  int searchFrom = 0;
  bool closed = false;
  int sortMode = g_cfg.sort;

  // отрисовка прогресса передачи снизу
  struct Progress
  {
    bool on = false;
    std::string title;
    long long done = 0, total = 0;
    double speed = 0; // байт в секунду
    double eta = -1;  // секунд до конца, -1 = пока хз
  } prog;

  // проверка статуса подключения (жив или нет)
  // если нет - подключаемся заново
  bool ensureConnection()
  {
    if (runCapture({"ssh", "-o", ctlOpt, "-O", "check", "--", tgt.ssh_host}, nullptr) == 0)
      return true;

    unlink(ctlFile.c_str()); // остался от упавшего подключения - мешает создать новый
    return runInTerminal({"ssh", "-o", ctlOpt, "-o", "ControlMaster=yes", "-o", "ControlPersist=30m", "-N", "-f", "--",
                          tgt.ssh_host},
                         "Connecting to " + tgt.ssh_host + "...") == 0;
  }

  void closeConnection()
  {
    if (closed)
      return;
    closed = true;
    runCapture({"ssh", "-o", ctlOpt, "-O", "exit", "--", tgt.ssh_host}, nullptr);
    unlink(ctlFile.c_str());
  }

  // ssh через уже открытое подключение
  // BatchMode - если подключение умерло = сразу ошибка
  std::vector<std::string> sshCmd(const std::string &script)
  {
    return {"ssh",
            "-o",
            ctlOpt,
            "-o",
            "ControlMaster=no",
            "-o",
            "BatchMode=yes",
            "--",
            tgt.ssh_host,
            "sh -c " + shq(script)};
  }

  int remoteSh(const std::string &script, std::string &out)
  {
    if (!ensureConnection())
      return 255;

    out.clear();
    return runCapture(sshCmd(script), &out);
  }

  // сколько весят директории на сервере; -1 если хз
  long long remoteDu(const std::vector<std::string> &paths)
  {
    std::string script = "du -sbc --";
    for (const auto &p : paths) script += " " + shq(p);
    script += " 2>/dev/null | tail -n 1";
    std::string out;

    if (remoteSh(script, out) == 0 && !out.empty() && isdigit((unsigned char)out[0]))
      return atoll(out.c_str());

    return -1;
  }

  bool readRemoteDir(const std::string &path, std::vector<Entry> &out, std::string &real, std::string &err)
  {
    std::string cdTo = (!path.empty() && path[0] == '-') ? "./" + path : path;
    std::string script =
        (path.empty() ? std::string("cd") : "cd " + shq(cdTo)) +
        " 2>&1 || exit 3\n"
        "pwd\n"
        "find . -mindepth 1 -maxdepth 1 -printf '%y\\t%Y\\t%m\\t%u\\t%g\\t%s\\t%T@\\t%f\\0' 2>/dev/null || exit 4\n";
    std::string data;
    int rc = remoteSh(script, data);

    if (rc == 3)
    {
      err = "Server: " + trim(data);
      return false;
    }

    if (rc == 4)
    {
      err = "Cannot read directory on the server (no permission or find without -printf)";
      return false;
    }

    if (rc == 255)
    {
      err = "No connection to the server";
      return false;
    }

    if (rc != 0)
    {
      err = "Server error, code " + std::to_string(rc);
      return false;
    }

    size_t nl = data.find('\n');

    if (nl == std::string::npos)
    {
      err = "Unexpected response from the server";
      return false;
    }

    real = data.substr(0, nl);
    out.clear();
    size_t pos = nl + 1;

    while (pos < data.size())
    {
      size_t end = data.find('\0', pos);

      if (end == std::string::npos)
        end = data.size();

      std::string rec = data.substr(pos, end - pos);
      pos = end + 1;
      std::string f[8];
      size_t s = 0;
      bool ok = true;

      for (int i = 0; i < 7 && ok; ++i)
      {
        size_t t = rec.find('\t', s);

        if (t == std::string::npos)
          ok = false;

        else
        {
          f[i] = rec.substr(s, t - s);
          s = t + 1;
        }
      }

      if (!ok)
        continue;
      f[7] = rec.substr(s);

      if (f[7].empty())
        continue;

      Entry e;
      e.name = f[7];
      e.mtime = atoll(f[6].c_str()); // "1695481234.1234567890" - дробную часть отбрасываем
      e.type = f[0].empty() ? '?' : f[0][0];
      e.is_dir = (f[1] == "d");
      e.mode = (unsigned)strtoul(f[2].c_str(), nullptr, 8) & 0777;
      e.owner = f[3];
      e.group = f[4];
      e.size = atoll(f[5].c_str());
      out.push_back(e);
    }
    return true;
  }

  long long remoteSize(const Pane &src, const std::vector<std::string> &names, const std::vector<std::string> &paths)
  {
    long long du = remoteDu(paths);

    if (du >= 0)
      return du;

    long long sum = 0;

    for (const auto &e : src.files)
      if (std::find(names.begin(), names.end(), e.name) != names.end())
        sum += e.size;

    return sum;
  }

  bool loadPane(Pane &p, const std::string &target, const std::string &focus)
  {
    std::vector<Entry> tmp;
    std::string err, real = target;
    bool ok = p.remote ? readRemoteDir(target, tmp, real, err) : readLocalDir(target, tmp, err);

    if (!ok)
    {
      status = err;
      return false;
    }

    sortEntries(tmp, sortMode);

    if (real != p.cwd)
      p.marked.clear();

    p.cwd = real;
    p.files.swap(tmp);
    std::set<std::string> alive;

    for (const auto &e : p.files)
      if (p.marked.count(e.name))
        alive.insert(e.name);

    p.marked.swap(alive);
    p.highlight = 0;
    p.top = 0;

    for (size_t i = 0; i < p.files.size(); ++i)
      if (p.files[i].name == focus)
      {
        p.highlight = (int)i;
        break;
      }

    return true;
  }

  void reloadPane(Pane &p)
  {
    std::string focus = p.files.empty() ? "" : p.files[p.highlight].name;
    int oldTop = p.top;
    if (loadPane(p, p.cwd, focus))
      p.top = oldTop;
  }

  // отмеченные
  // а если ничего не отмечено то по позиции курсора
  std::vector<std::string> selection(const Pane &p, bool onlyCurrent)
  {
    std::vector<std::string> names;

    if (!onlyCurrent && !p.marked.empty())
      names.assign(p.marked.begin(), p.marked.end());

    else if (!p.files.empty())
      names.push_back(p.files[p.highlight].name);

    return names;
  }

  static std::string describe(const std::vector<std::string> &names)
  { return names.size() == 1 ? names[0] : std::to_string(names.size()) + " items"; }

  // вопрос в нижней строке; ждем одну из клавиш; Esc вернет 27
  int ask(const std::string &question, const std::string &keys)
  {
    status = question;
    draw();
    int ch;

    while ((ch = getch()) != 27 && ch != 3 && (ch < 0 || ch > 255 || keys.find((char)ch) == std::string::npos))
      if (ch == KEY_RESIZE)
        draw();

    status.clear();
    return ch == 3 ? 27 : ch; // Ctrl+C - то же, что Esc
  }

  // узнаем что уже есть в папке назначения и выкидываем пропущенное
  // false - передавать нечего или Esc
  bool askOverwrite(const Pane &dst, std::vector<std::string> &names)
  {
    std::vector<std::string> keep;
    bool all = false, none = false;

    for (const auto &n : names)
    {
      const Entry *old = nullptr;

      for (const auto &e : dst.files)
        if (e.name == n)
        {
          old = &e;
          break;
        }

      if (old == nullptr || all)
      {
        keep.push_back(n);
        continue;
      }

      if (none)
        continue;

      std::string q = old->is_dir ? "Folder " + n + "/ exists, files inside will be overwritten"
                                  : n + " already exists (" + sizeText(old->size) + ", " + formatDate(old->mtime) + ")";
      int k = ask(q + ":  y overwrite  n skip  a overwrite all  s skip all  Esc cancel", "ynas");

      if (k == 27)
      {
        status = "Cancelled";
        return false;
      }

      if (k == 'y' || k == 'a')
        keep.push_back(n);

      all = all || k == 'a';
      none = none || k == 's';
    }

    if (keep.empty())
    {
      status = "Nothing to transfer: everything was skipped";
      return false;
    }

    names.swap(keep);
    return true;
  }

  // "  1,234,567  45%  1.23MB/s  0:00:12" (строка --info=progress2 от rsync) -> 1234567
  static bool parseRsyncProgress(const std::string &line, long long &bytes)
  {
    std::string num;
    size_t i = 0;

    while (i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == ','))
    {
      if (line[i] != ',')
        num += line[i];
      ++i;
    }

    if (num.empty() || line.find('%', i) == std::string::npos)
      return false;

    bytes = atoll(num.c_str());
    return true;
  }

  // "huge.bin   45% 3686MB 804.1MB/s   00:05 ETA" (строка прогресса scp) -> имя файла и сколько его уже передано
  static bool parseScpProgress(const std::string &line, std::string &name, long long &bytes)
  {
    std::istringstream iss(line);
    std::vector<std::string> tok;

    for (std::string t; iss >> t;) tok.push_back(t);

    for (size_t i = 1; i + 1 < tok.size(); ++i)
    {
      const std::string &pct = tok[i];

      if (pct.size() < 2 || pct.back() != '%' || pct.find_first_not_of("0123456789") != pct.size() - 1)
        continue;

      char *end = nullptr;
      double v = strtod(tok[i + 1].c_str(), &end);
      std::string unit = end ? end : "";
      const char *units[] = {"B", "KB", "MB", "GB", "TB"};

      for (int k = 0; k < 5; ++k)
        if (unit == units[k])
          for (int j = 0; j < k; ++j) v *= 1024;

      name.clear();

      for (size_t j = 0; j < i; ++j) name += (j ? " " : "") + tok[j];

      bytes = (long long)v;
      return true;
    }

    return false;
  }

  // запуск scp/rsync без выхода из интерфейса; прогресс снизу
  // запуск в псевдотерминале
  int runWithProgress(const std::vector<std::string> &args, const std::string &title, long long total, bool rsyncOut,
                      std::string &firstMsg, bool &cancelled)
  {
    int master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname(master) == nullptr)
    {
      if (master >= 0)
        close(master);
      return -1;
    }

    std::string slaveName = ptsname(master);
    pid_t pid = fork();

    if (pid < 0)
    {
      close(master);
      return -1;
    }

    if (pid == 0)
    {
      setsid();
      int slave = open(slaveName.c_str(), O_RDWR);

      if (slave < 0)
        _exit(127);

      struct winsize ws{};
      ws.ws_row = 24;
      ws.ws_col = 512;
      ioctl(slave, TIOCSWINSZ, &ws);
      dup2(slave, 0);
      dup2(slave, 1);
      dup2(slave, 2);
      close(slave);
      close(master);
      signal(SIGINT, SIG_DFL);
      setenv("LC_ALL", "C", 1);
      execArgs(args);
      perror(args[0].c_str());
      _exit(127);
    }

    g_cancel = 0;
    void (*oldInt)(int) = signal(SIGINT, onSigint);
    nodelay(stdscr, TRUE);

    prog = Progress();
    prog.on = true;
    prog.title = title;
    prog.total = total;
    cancelled = false;

    // скорость по последним 5 секундам
    std::vector<std::pair<double, long long>> samples;
    double lastDraw = -1e9;
    std::string buf;
    // у scp прогресс по каждому файлу отдельно поэтому складываем перенесенные файлы и текущий
    std::string curName;
    long long doneFiles = 0, curBytes = 0;

    for (bool eof = false; !eof;)
    {
      pollfd pfd{master, POLLIN, 0};

      if (poll(&pfd, 1, 200) > 0)
      {
        char tmp[4096];
        ssize_t n = read(master, tmp, sizeof(tmp));

        if (n > 0)
          buf.append(tmp, n);

        else if (n == 0 || errno != EINTR)
          eof = true;
      }

      // rsync обновляет строку прогресса через \r поэтому режем и по \r и по \n
      size_t cut;

      while ((cut = buf.find_first_of("\r\n")) != std::string::npos)
      {
        std::string line = trim(buf.substr(0, cut));
        buf.erase(0, cut + 1);
        long long b;
        std::string nm;

        if (rsyncOut && parseRsyncProgress(line, b))
          prog.done = b;

        else if (!rsyncOut && parseScpProgress(line, nm, b))
        {
          if (nm != curName)
          {
            doneFiles += curBytes;
            curName = nm;
          }

          curBytes = b;
          prog.done = doneFiles + curBytes;
        }

        else if (firstMsg.empty() && !line.empty())
          firstMsg = line;
      }

      double t = nowSec();

      if (total > 0 && prog.done > total)
        prog.done = total;

      samples.push_back({t, prog.done});

      while (samples.size() > 2 && t - samples.front().first > 5) samples.erase(samples.begin());

      double dt = t - samples.front().first;

      // первые пару секунд надо выждать
      if (dt >= 2)
      {
        prog.speed = (prog.done - samples.front().second) / dt;
        prog.eta = prog.speed > 0 && total > 0 ? (total - prog.done) / prog.speed : -1;
      }

      int ch;

      while ((ch = getch()) != ERR)
        if (g_cfg.cancelKeys.count(ch) || ch == 3) // Ctrl+C отменяет всегда, даже если в конфиге его убрали
          g_cancel = 1;

      if (g_cancel && !cancelled)
      {
        cancelled = true;
        kill(-pid, SIGTERM);
      }

      if (t - lastDraw >= 0.25)
      {
        draw();
        lastDraw = t;
      }
    }

    close(master);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    nodelay(stdscr, FALSE);
    signal(SIGINT, oldInt);
    prog.on = false;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  }

  bool removePaths(bool remote, const std::vector<std::string> &paths, std::string &err)
  {
    if (remote)
    {
      std::string script = "rm -rf --";
      for (const auto &p : paths) script += " " + shq(p);
      std::string out;
      int rc = remoteSh(script + " 2>&1", out);

      if (rc != 0)
        err = out.empty() ? "rm returned " + std::to_string(rc) : firstLine(out);

      return rc == 0;
    }

    for (const auto &p : paths)
      if (!removeLocal(p, err))
        return false;

    return true;
  }

  // при отмене предлагаем убрать то что успели перекинуть
  // удаляем только то чего в директории не было
  // если то что было перезаписали - не трогаем
  std::string afterCancel(const Pane &dst, const std::vector<std::string> &names, const std::set<std::string> &existed,
                          bool useRsync)
  {
    std::vector<std::string> fresh;
    size_t overwritten = 0;

    for (const auto &n : names)
    {
      if (existed.count(n))
        ++overwritten;
      else
        fresh.push_back(joinPath(dst.cwd, n));
    }

    std::string note;

    if (overwritten > 0)
      note = useRsync ? "; files you chose to overwrite are untouched, send again to resume"
                      : "; " + std::to_string(overwritten) + " overwritten item(s) already lost the old version";

    if (fresh.empty())
      return "Cancelled" + note;

    // папку для недокачанного rsync тоже создал сам, если ее не было
    if (useRsync && !existed.count(PARTIAL_DIR))
      fresh.push_back(joinPath(dst.cwd, PARTIAL_DIR));

    if (ask("Cancelled. Remove what was already copied (only new items)?  y remove  n keep", "yn") != 'y')
      return "Cancelled: partly copied files are left in the destination" + note;

    std::string err;

    if (!removePaths(dst.remote, fresh, err))
      return "Cancelled, but cleanup failed: " + err;

    return "Cancelled: removed partly copied files" + note;
  }

  void transfer(bool onlyCurrent)
  {
    Pane &src = panes[active];
    Pane &dst = panes[1 - active];
    bool upload = !src.remote;
    std::vector<std::string> names = selection(src, onlyCurrent);

    if (!ensureConnection())
    {
      status = "No connection to the server";
      return;
    }

    reloadPane(dst);

    if (!askOverwrite(dst, names))
      return;

    // что уже было в директории назначения до перекидки
    std::set<std::string> existed;
    for (const auto &e : dst.files) existed.insert(e.name);

    std::vector<std::string> paths;
    for (const auto &n : names) paths.push_back(joinPath(src.cwd, n));

    status = "Calculating size...";
    draw();
    long long size = upload ? localSize(paths, LLONG_MAX) : remoteSize(src, names, paths);
    bool useRsync = g_cfg.rsyncFrom != RSYNC_NEVER && size >= g_cfg.rsyncFrom && rsyncLocal && rsyncRemote;
    std::string sshOpts = "ssh -o " + ctlOpt + " -o ControlMaster=no -o BatchMode=yes";
    std::vector<std::string> args;

    if (useRsync)
      // -a сохраняет права и даты, --partial-dir позволяет докачать после обрыва и не портит старый файл
      // -s не дает shell на сервере "поделить" имена с пробелами
      args = {"rsync", "-a",    "-s", std::string("--partial-dir=") + PARTIAL_DIR, "--info=progress2",
              "-e",    sshOpts, "--"};

    else
      args = {"scp", "-r", "-o", ctlOpt, "-o", "ControlMaster=no", "-o", "BatchMode=yes", "--"};

    for (const auto &p : paths) args.push_back(upload ? p : tgt.scp_host + ":" + p);
    args.push_back(upload ? tgt.scp_host + ":" + dirArg(dst.cwd) : dirArg(dst.cwd));
    std::string what = describe(names);
    std::string how = useRsync ? "rsync" : "scp";

    std::string msg;
    bool cancelled = false;
    double start = nowSec();
    int rc = runWithProgress(args, (upload ? "Uploading: " : "Downloading: ") + what + " (" + how + ")", size, useRsync,
                             msg, cancelled);
    double took = nowSec() - start;

    if (cancelled)
      status = afterCancel(dst, names, existed, useRsync);

    else if (rc == 0)
    {
      src.marked.clear();
      status = (upload ? "Uploaded: " : "Downloaded: ") + what + " via " + how + ", " + sizeText(size) + " in " +
               formatDuration(took);

      if (took >= 1)
        status += " (" + sizeText((long long)(size / took)) + "/s)";
    }

    else
      status = "Failed (" + how + " returned " + std::to_string(rc) + "): " + (msg.empty() ? what : msg);

    std::string result = status;
    reloadPane(dst);
    status = result;
  }

  void viewCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
      return;

    const Entry &e = p.files[p.highlight];

    if (e.is_dir)
    {
      status = "Cannot view a folder";
      return;
    }

    std::string path = joinPath(p.cwd, e.name);

    if (p.remote)
    {
      if (!ensureConnection())
      {
        status = "No connection to the server";
        return;
      }

      std::string script = "if command -v less >/dev/null 2>&1; then exec less -- \"$1\"; else exec more -- \"$1\"; fi";
      runInTerminal({"ssh", "-o", ctlOpt, "-o", "ControlMaster=no", "-t", "--", tgt.ssh_host,
                     "sh -c " + shq(script) + " sh " + shq(path)},
                    "Viewing: " + tgt.ssh_host + ":" + path);
      return;
    }

    const char *pager = getenv("PAGER");
    std::string cmd = (pager && *pager) ? pager : (inPath("less") ? "less" : "more");
    runInTerminal({"sh", "-c", cmd + " \"$1\"", "sh", path}, "Viewing: " + path);
  }

  static std::string editorCmd()
  {
    for (const char *var : {"VISUAL", "EDITOR"})
    {
      const char *v = getenv(var);

      if (v && *v)
        return v;
    }

    return inPath("nano") ? "nano" : "vi";
  }

  // редактирование файлов
  // файл с сервера скачиваем во временную папку и открываем в редакторе
  // если что то поменяли - заливаем через "cat >"
  void editCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
      return;

    Entry e = p.files[p.highlight];

    if (e.is_dir)
    {
      status = "Cannot edit a folder";
      return;
    }

    std::string path = joinPath(p.cwd, e.name);
    std::string editor = editorCmd();

    if (!p.remote)
    {
      runInTerminal({"sh", "-c", editor + " \"$1\"", "sh", path}, "Editing: " + path);
      reloadPane(p);
      return;
    }

    if (!ensureConnection())
    {
      status = "No connection to the server";
      return;
    }

    const char *tmpEnv = getenv("TMPDIR");
    std::string tmpl = std::string(tmpEnv && *tmpEnv ? tmpEnv : "/tmp") + "/scpanel-XXXXXX";
    std::vector<char> dirBuf(tmpl.begin(), tmpl.end());
    dirBuf.push_back('\0');

    if (mkdtemp(dirBuf.data()) == nullptr)
    {
      status = std::string("Cannot create a temp folder: ") + strerror(errno);
      return;
    }

    std::string dir = dirBuf.data(), local = dir + "/" + e.name, err;
    int fd = open(local.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    status = "Downloading " + e.name + "...";
    draw();
    int rc = fd < 0 ? -1 : runPiped(sshCmd("cat -- " + shq(path)), -1, fd, &err);

    if (fd >= 0)
      close(fd);

    std::string before, after, ignore;

    if (rc != 0 || !readFile(local, before))
    {
      status = "Cannot read " + e.name + ": " + (err.empty() ? "cat returned " + std::to_string(rc) : firstLine(err));
      removeLocal(dir, ignore);
      return;
    }

    runInTerminal({"sh", "-c", editor + " \"$1\"", "sh", local}, "Editing: " + tgt.ssh_host + ":" + path);

    if (!readFile(local, after) || after == before)
    {
      status = "No changes: " + e.name;
      removeLocal(dir, ignore);
      return;
    }

    // пока правили, подключение могло закрыться (оно живет 30 минут без дела)
    int in = ensureConnection() ? open(local.c_str(), O_RDONLY) : -1;
    err.clear();
    rc = in < 0 ? -1 : runPiped(sshCmd("cat > " + shq(path)), in, -1, &err);

    if (in >= 0)
      close(in);

    if (rc != 0)
    {
      // временный файл с правками оставляем; выводим путь до него
      status = "Upload failed (" + (err.empty() ? "code " + std::to_string(rc) : firstLine(err)) +
               "), your edit is kept in " + local;
      return;
    }

    removeLocal(dir, ignore);
    reloadPane(p);
    status = "Saved: " + tgt.ssh_host + ":" + path;
  }

  void deleteSelected()
  {
    Pane &p = panes[active];
    std::vector<std::string> names = selection(p, false);

    if (names.empty())
      return;

    std::string what = describe(names);

    if (ask("Delete " + what + (p.remote ? " on the server" : "") + "?  y yes  n no", "yn") != 'y')
    {
      status = "Cancelled";
      return;
    }

    std::string err;
    std::vector<std::string> paths;
    for (const auto &n : names) paths.push_back(joinPath(p.cwd, n));
    removePaths(p.remote, paths, err);

    int keepAt = p.highlight;
    p.marked.clear();
    loadPane(p, p.cwd, "");
    p.highlight = std::max(0, std::min(keepAt, (int)p.files.size() - 1));
    status = err.empty() ? "Deleted: " + what : "Delete failed: " + err;
  }

  void renameCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
      return;

    std::string old = p.files[p.highlight].name;
    std::string name = inputLine("Rename to: ", old);

    if (name.empty() || name == old)
      return;

    std::string from = joinPath(p.cwd, old), to = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote)
    {
      std::string out;
      std::string script = "if [ -e " + shq(to) + " ] || [ -L " + shq(to) +
                           " ]; then echo 'already exists'; exit 1; fi\n" + "mv -- " + shq(from) + " " + shq(to) +
                           " 2>&1";

      int rc = remoteSh(script, out);

      if (rc != 0)
        err = out.empty() ? "mv returned " + std::to_string(rc) : firstLine(out);
    }

    else
    {
      struct stat st{};

      if (lstat(to.c_str(), &st) == 0)
        err = "already exists";

      else if (rename(from.c_str(), to.c_str()) != 0)
        err = strerror(errno);
    }

    if (!err.empty())
    {
      status = "Rename failed: " + err;
      return;
    }

    p.marked.erase(old);
    loadPane(p, p.cwd, name);
    status = "Renamed: " + old + " -> " + name;
  }

  void makeDir()
  {
    Pane &p = panes[active];
    std::string name = inputLine("New folder: ");

    if (name.empty())
      return;

    std::string path = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote)
    {
      std::string out;
      std::string script = "if [ -e " + shq(path) + " ]; then echo 'already exists'; exit 1; fi\n" + "mkdir -p -- " +
                           shq(path) + " 2>&1";
      int rc = remoteSh(script, out);

      if (rc != 0)
        err = out.empty() ? "mkdir returned " + std::to_string(rc) : firstLine(out);
    }

    else
    {
      struct stat st{};

      if (lstat(path.c_str(), &st) == 0)
        err = "already exists";

      else
        makeDirs(path, err);
    }

    if (!err.empty())
    {
      status = "Cannot create folder: " + err;
      return;
    }

    // переходим к созданной директории (для "a/b" - на "a")
    loadPane(p, p.cwd, name.substr(0, name.find('/')));
    status = "Created: " + name;
  }
  // создаем пустой файл; уже существующий не трогаем
  void makeFile()
  {
    Pane &p = panes[active];
    std::string name = inputLine("New file: ");

    if (name.empty())
      return;

    std::string path = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote)
    {
      std::string out;
      std::string script = "if [ -e " + shq(path) + " ] || [ -L " + shq(path) +
                           " ]; then echo 'already exists'; exit 1; fi\n" + "( : > " + shq(path) + " ) 2>&1";
      int rc = remoteSh(script, out);

      if (rc != 0)
        err = out.empty() ? "code " + std::to_string(rc) : firstLine(out);
    }

    else
    {
      int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);

      if (fd < 0)
        err = errno == EEXIST ? "already exists" : strerror(errno);
      else
        close(fd);
    }

    if (!err.empty())
    {
      status = "Cannot create file: " + err;
      return;
    }

    loadPane(p, p.cwd, name.substr(0, name.find('/')));
    status = "Created: " + name;
  }

  // ~ - домашняя папка
  // путь без / в начале - от текущей папки
  void gotoPath()
  {
    Pane &p = panes[active];
    std::string in = inputLine("Go to: ", p.cwd);

    if (in.empty() || in == p.cwd)
      return;

    std::string target;

    if (p.remote)
    {
      if (in == "~" || in == "~/")
        target = "";

      else if (in.compare(0, 2, "~/") == 0)
        target = in.substr(2);

      else
        target = in[0] == '/' ? in : joinPath(p.cwd, in);
    }

    else
    {
      const char *home = getenv("HOME");

      if (home && (in == "~" || in.compare(0, 2, "~/") == 0))
        in = home + in.substr(1);

      target = absolutePath(in[0] == '/' ? in : joinPath(p.cwd, in));

      if (target.empty())
      {
        status = "No such folder: " + in;
        return;
      }
    }

    loadPane(p, target, "");
  }

  void cycleSort()
  {
    sortMode = (sortMode + 1) % 3;

    for (Pane &p : panes)
    {
      std::string focus = p.files.empty() ? "" : p.files[p.highlight].name;
      sortEntries(p.files, sortMode);

      for (size_t i = 0; i < p.files.size(); ++i)
        if (p.files[i].name == focus)
          p.highlight = (int)i;
    }

    status = std::string("Sort by ") + SORT_NAMES[sortMode];
  }

  // список всех биндов с учетом конфига
  void showHelp()
  {
    int n = ACT_COUNT - 1, top = 0;

    while (true)
    {
      erase();
      int rows = getmaxy(stdscr), cols = getmaxx(stdscr);
      int perCol = std::max(1, rows - 3);
      int ncol = n > perCol && cols >= 104 ? 2 : 1, colW = cols / ncol;
      bool scroll = n > perCol * ncol;
      top = scroll ? std::max(0, std::min(top, n - perCol)) : 0;
      attron(A_BOLD);
      mvaddstr(0, 0, fitCols("Keys (change them in ~/.scpanel.config, see README)", cols).c_str());
      attroff(A_BOLD);

      for (int i = top; i < n; ++i)
      {
        int c = (i - top) / perCol, r = (i - top) % perCol;

        if (c >= ncol)
          break;

        int a = i + 1;
        mvaddstr(r + 2, c * colW, fitCols("  " + fitCols(keysText(a), 18) + "  " + ACTIONS[a].help, colW).c_str());
      }

      mvaddstr(rows - 1, 0,
               fitCols(scroll ? "Up/Down scroll  any other key - back" : "Press any key", cols - 1).c_str());
      refresh();
      int ch = getch();

      if (ch == KEY_RESIZE)
        continue;

      if (scroll && (ch == KEY_UP || ch == KEY_DOWN || ch == KEY_PPAGE || ch == KEY_NPAGE))
      {
        top += ch == KEY_UP ? -1 : ch == KEY_DOWN ? 1 : ch == KEY_PPAGE ? -perCol : perCol;
        continue;
      }

      return;
    }
  }

  // 1 - ищем вниз, -1 - вверх (p - предыдущее совпадение); на последнем совпадении переходим на другой конец
  bool findMatch(Pane &p, const std::string &q, int from, int step = 1)
  {
    if (q.empty() || p.files.empty())
      return false;

    std::wstring wq = lowerW(q);
    int n = (int)p.files.size();

    for (int k = 0; k < n; ++k)
    {
      int i = ((from + step * k) % n + n) % n;

      if (lowerW(p.files[i].name).find(wq) != std::wstring::npos)
      {
        p.highlight = i;
        return true;
      }
    }
    return false;
  }

  // поиск во время ввода
  // курсор прыгает на первое совпадение
  void searchKey(int ch)
  {
    if (ch == 27)
    {
      searching = false;
      return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
    {
      searching = false;

      if (!search.empty())
        lastSearch = search;

      return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
      popUtf8(search);

    else if (ch >= 32 && ch < 256)
      search += (char)ch;

    else
      return;

    searchMiss = !search.empty() && !findMatch(panes[active], search, searchFrom);
  }

  void drawPane(Pane &p, int x, int w, int listH, bool act)
  {
    std::string title = p.remote ? " " + tgt.ssh_host + ":" + p.cwd : " " + p.cwd;
    const char *sortTag[] = {"", "  [newest]", "  [largest]"};
    title += sortTag[sortMode];
    int hattr = act ? A_REVERSE : A_BOLD;
    attron(hattr);
    mvaddstr(0, x, fitCols(title, w).c_str());
    attroff(hattr);

    if (p.highlight < p.top)
      p.top = p.highlight;

    if (p.highlight >= p.top + listH)
      p.top = p.highlight - listH + 1;

    if (p.files.empty())
      mvaddstr(1, x, fitCols("  (empty)", w).c_str());

    const int sizeW = 7;

    for (int r = 0; r < listH && p.top + r < (int)p.files.size(); ++r)
    {
      int i = p.top + r;
      const Entry &e = p.files[i];
      bool mk = p.marked.count(e.name) > 0;
      std::string name = e.name + (e.is_dir ? "/" : "");
      std::string line = mk ? "* " : "  ";

      if (w >= 2 + sizeW + 6)
      {
        std::string size = e.is_dir ? "" : humanSize(e.size);
        line += fitCols(name, w - 2 - sizeW) + std::string(sizeW - size.size(), ' ') + size;
      }
      else
        line += fitCols(name, w - 2);

      int attr = mk ? A_BOLD : 0;

      if (i == p.highlight)
        attr |= act ? A_REVERSE : A_UNDERLINE;

      attron(attr);
      mvaddstr(r + 1, x, line.c_str());
      attroff(attr);
    }
  }

  // две нижние строки во время передачи:
  // [=========>          ]  45%  123M / 274M
  // Uploading: backup.tar (rsync)  12M/s  ETA 0:12  Esc cancel
  void drawProgress(int rows, int cols)
  {
    int pct = prog.total > 0 ? (int)(prog.done * 100 / prog.total) : 0;
    pct = std::max(0, std::min(100, pct));
    std::string right = "  " + std::to_string(pct) + "%  " + sizeText(prog.done) + " / " + sizeText(prog.total);
    int barW = std::max(3, cols - 1 - colsOf(right) - 2);
    int fill = barW * pct / 100;
    std::string bar = std::string(fill, '=');

    if (fill < barW)
      bar += (fill > 0 ? ">" : " ") + std::string(barW - fill - 1, ' ');

    attron(A_BOLD);
    mvaddstr(rows - 2, 0, fitCols("[" + bar + "]" + right, cols).c_str());
    attroff(A_BOLD);
    std::string speed = prog.speed > 0 ? sizeText((long long)prog.speed) + "/s" : "--/s";
    std::string eta = prog.eta >= 0 ? formatDuration(prog.eta) : "--";
    mvaddstr(rows - 1, 0, fitCols(prog.title + "  " + speed + "  ETA " + eta + "  Esc cancel", cols - 1).c_str());
  }

  void draw()
  {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    if (rows < 5 || cols < 30)
    {
      mvaddstr(0, 0, "Window is too small");
      refresh();
      return;
    }

    int listH = rows - 3; // заголовок сверху, две строки снизу
    int lw = (cols - 1) / 2;
    drawPane(panes[0], 0, lw, listH, active == 0);
    drawPane(panes[1], lw + 1, cols - lw - 1, listH, active == 1);
    mvvline(0, lw, ACS_VLINE, listH + 1);
    // инфа о файле под курсором
    Pane &p = panes[active];
    std::string info;

    if (!p.files.empty())
    {
      const Entry &e = p.files[p.highlight];
      info = permString(e.type, e.mode) + "  " + e.owner + " " + e.group + "  " + humanSize(e.size) + "  " +
             formatDate(e.mtime) + "  " + e.name;
    }

    if (!p.marked.empty())
      info = "Marked: " + std::to_string(p.marked.size()) + "  |  " + info;

    if (prog.on)
    {
      drawProgress(rows, cols);
      refresh();
      return;
    }

    attron(A_DIM);
    mvaddstr(rows - 2, 0, fitCols(info, cols).c_str());
    attroff(A_DIM);
    std::string bottom;

    if (searching)
      bottom = "Search: " + search + "_" + (searchMiss ? "   (no matches)" : "");

    else
      bottom = status.empty() ? helpLine() : status;
    // В последнюю клетку экрана не пишем
    // а то curses от этого может прокрутить экран
    mvaddstr(rows - 1, 0, fitCols(bottom, cols - 1).c_str());
    refresh();
  }
};

int main(int argc, char *argv[])
{
  if (argc > 2)
  {
    printf("Usage: %s [user@host[:path]]\n", argv[0]);
    return 1;
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
    mkdir((std::string(home) + "/.ssh").c_str(), 0700); // файл подключения

  loadConfig();
  initscr();
  // raw, а не cbreak: Ctrl+C приходит как обычная клавиша, и мы выходим по-человечески
  // (закрываем подключение и возвращаем терминал в порядок), а не умираем посреди отрисовки
  raw();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  set_escdelay(25); // чтобы Esc срабатывал сразу
  std::string target = (argc == 2) ? argv[1] : pickServer();

  if (target.empty())
  {
    leaveCurses();
    return 0;
  }

  App app(parseTarget(target), local);
  // сервер не из ~/.ssh/config - после подключения спросим, сохранить ли его туда
  std::vector<std::string> known = sshConfigHosts();
  std::string hostOnly = app.hostName();
  app.offerSave = std::find(known.begin(), known.end(), hostOnly) == known.end() &&
                  std::find(known.begin(), known.end(), parseTarget(target).ssh_host) == known.end();
  int rc = app.run();
  leaveCurses();

  if (!app.fatal.empty())
    fprintf(stderr, "%s\n", app.fatal.c_str());
  return rc;
}