#pragma once

#include "actions.hpp"
#include "utils.hpp"

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
  {
    if (c != '[' && c != ']')
    {
      r.ssh_host += c;
    }
  }

  return r;
}

// настройка конфига
static std::string lower(std::string s)
{
  for (char &c : s)
  {
    c = (char)tolower((unsigned char)c);
  }

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
  {
    return {(unsigned char)name[0]};
  }

  std::string n = lower(name);

  if (n.size() == 6 && n.compare(0, 5, "ctrl+") == 0 && isalpha((unsigned char)n[5]))
  {
    return {n[5] - 'a' + 1};
  }

  if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1]))
  {
    int k = atoi(n.c_str() + 1);

    if (k >= 1 && k <= 12 && n == "f" + std::to_string(k))
    {
      return {KEY_F(k)};
    }
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
{
  return homeDir() + "/.scpanel.config";
}

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

    for (std::string k; iss >> k;)
    {
      c.keys[a].push_back(k);
    }
  }

  std::ifstream f(configPath());
  std::string line, section;
  int no = 0;

  while (std::getline(f, line))
  {
    ++no;
    line = trim(line);

    if (line.empty() || line[0] == '#' || line[0] == ';')
    {
      continue;
    }

    std::string where = "~/.scpanel.config line " + std::to_string(no) + ": ";

    if (line[0] == '[')
    {
      section = lower(trim(line.substr(1, line.find(']') - 1)));

      if (section != "keys" && section != "options")
      {
        c.warnings.push_back(where + "unknown section [" + section + "]");
      }

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

      while (a < ACT_COUNT && name != ACTIONS[a].name)
      {
        ++a;
      }

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
        {
          c.warnings.push_back(where + "unknown key '" + k + "'");
        }

        else
        {
          c.keys[a].push_back(k);
        }
      }
    }

    else if (section == "options" && name == "sort")
    {
      std::string v = lower(value);
      c.sort = v == "date" ? 1 : v == "size" ? 2 : 0;

      if (v != "name" && v != "date" && v != "size")
      {
        c.warnings.push_back(where + "sort must be name, date or size");
      }
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
      {
        c.rsyncFrom = RSYNC_NEVER;
      }

      else if (end == v.c_str() || n < 0 || !mult.count(unit))
      {
        c.warnings.push_back(where + "rsync_from must be a size like 100M, 1G, 0 or never");
      }

      else
      {
        c.rsyncFrom = (long long)(n * mult.at(unit));
      }
    }

    else
    {
      c.warnings.push_back(where + "unknown setting '" + name + "'");
    }
  }

  // сначала раскидываем кнопки по дефолту потом по конфигу
  // конфиг главнее
  for (int pass = 0; pass < 2; ++pass)
  {
    for (int a = 1; a < ACT_CANCEL; ++a)
    {
      if (custom[a] == (pass == 1))
      {
        for (const auto &k : c.keys[a])
        {
          for (int code : keyCodes(k))
          {
            c.keymap[code] = a;
          }
        }
      }
    }
  }

  // меняем подсказки для переназначенных кнопок
  for (int a = 1; a < ACT_CANCEL; ++a)
  {
    std::vector<std::string> kept;

    for (const auto &k : c.keys[a])
    {
      if (c.keymap[keyCodes(k)[0]] == a)
      {
        kept.push_back(k);
      }
    }

    c.keys[a].swap(kept);
  }

  for (const auto &k : c.keys[ACT_CANCEL])
  {
    for (int code : keyCodes(k))
    {
      c.cancelKeys.insert(code);
    }
  }

  g_cfg = c;
}

static int actionFor(int ch)
{
  auto it = g_cfg.keymap.find(ch);
  return it == g_cfg.keymap.end() ? ACT_NONE : it->second;
}

// "s / F5" - вывод клавиш действий
static std::string keysText(int a)
{
  std::string r;

  for (const auto &k : g_cfg.keys[a])
  {
    r += (r.empty() ? "" : " / ") + k;
  }

  return r.empty() ? "(none)" : r;
}

static std::string helpLine()
{
  const std::pair<int, const char *> shown[] = {{ACT_HELP, "help"},     {ACT_PANEL, "panel"},   {ACT_MARK, "mark"},
                                                {ACT_SEND, "send"},     {ACT_EDIT, "edit"},     {ACT_VIEW, "view"},
                                                {ACT_DELETE, "delete"}, {ACT_SEARCH, "search"}, {ACT_QUIT, "quit"}};
  std::string r;

  for (const auto &s : shown)
  {
    if (!g_cfg.keys[s.first].empty())
    {
      r += (r.empty() ? "" : "  ") + g_cfg.keys[s.first][0] + " " + s.second;
    }
  }

  return r;
}
