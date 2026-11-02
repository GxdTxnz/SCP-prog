#pragma once

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

static std::string shq(const std::string &s)
{
  std::string r = "'";

  for (char c : s)
  {
    r += (c == '\'') ? std::string("'\\''") : std::string(1, c);
  }

  return r + "'";
}

// "/home/user/x" -> "/home/user", "/home" -> "/"
static std::string parentOf(const std::string &path)
{
  size_t pos = path.find_last_of('/');

  if (pos == 0 || pos == std::string::npos)
  {
    return "/";
  }

  return path.substr(0, pos);
}

// чтобы в корне не получалось "//etc"
static std::string joinPath(const std::string &dir, const std::string &name)
{
  return dir == "/" ? "/" + name : dir + "/" + name;
}

// "/tmp" -> "/tmp/", "/" -> "/"
static std::string dirArg(const std::string &dir)
{
  return dir == "/" ? "/" : dir + "/";
}

static std::string absolutePath(const std::string &path)
{
  char buf[PATH_MAX];

  if (realpath(path.c_str(), buf) == nullptr)
  {
    return "";
  }

  return buf;
}

static std::string trim(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\r\n");

  if (a == std::string::npos)
  {
    return "";
  }

  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// cтираем последний символ даже если это русская буква из двух байт
static void popUtf8(std::string &s)
{
  while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80)
  {
    s.pop_back();
  }

  if (!s.empty())
  {
    s.pop_back();
  }
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
  {
    snprintf(buf, sizeof(buf), "%lld", b);
  }

  else
  {
    snprintf(buf, sizeof(buf), v < 10 ? "%.1f%c" : "%.0f%c", v, units[i]);
  }

  return buf;
}

// то же но с "B" для небольших размеров
static std::string sizeText(long long b)
{
  return humanSize(b) + (b < 1024 ? "B" : "");
}

// 1695481234 -> "2023-09-23 18:00"
static std::string formatDate(long long t)
{
  if (t <= 0)
  {
    return "";
  }

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
  {
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  }

  else
  {
    snprintf(buf, sizeof(buf), "%lld:%02lld", s / 60, s % 60);
  }

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
    {
      return line;
    }
  }

  return "";
}

static bool readFile(const std::string &path, std::string &out)
{
  std::ifstream f(path, std::ios::binary);

  if (!f)
  {
    return false;
  }

  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// проверка наличия в PATH
static bool inPath(const std::string &prog)
{
  const char *env = getenv("PATH");

  if (env == nullptr)
  {
    return false;
  }

  std::stringstream ss(env);
  std::string dir;

  while (std::getline(ss, dir, ':'))
  {
    if (dir.empty())
    {
      dir = ".";
    }

    if (access((dir + "/" + prog).c_str(), X_OK) == 0)
    {
      return true;
    }
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
    {
      n = 1;
    }

    int w = bad ? -1 : wcwidth(wc);
    bool printable = !bad && w >= 0 && !iswcntrl(wc);

    if (!f(i, n, printable ? w : 1, printable))
    {
      return;
    }

    i += n;
  }
}

static int colsOf(const std::string &s)
{
  int total = 0;
  forEachChar(s, [&](size_t, size_t, int w, bool) { total += w; return true; });
  return total;
}

// подгон строки под кол-во клеток; длинную обрезаем с добавлением "…"; короткую добиваем пробелами
// символы которые невозможно вывести заменяем на "?"
static std::string fitCols(const std::string &s, int w)
{
  if (w <= 0)
  {
    return "";
  }

  bool cut = colsOf(s) > w;
  int limit = cut ? w - 1 : w;
  std::string out;
  int used = 0;

  forEachChar(s,
              [&](size_t i, size_t n, int cw, bool printable)
              {
                if (used + cw > limit)
                {
                  return false;
                }

                if (printable)
                {
                  out.append(s, i, n);
                }

                else
                {
                  out += '?';
                }

                used += cw;
                return true;
              });

  if (cut)
  {
    out += "…";
    ++used;
  }

  if (used < w)
  {
    out.append(w - used, ' ');
  }

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
    {
      n = 1;
    }

    out += (wchar_t)towlower(wc);
    i += n;
  }

  return out;
}
