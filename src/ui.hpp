#pragma once

#include "hosts.hpp"
#include "process.hpp"

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

      while (skip < shown.size() && ((unsigned char)shown[skip] & 0xC0) == 0x80)
      {
        ++skip;
      }

      shown.erase(0, skip);
    }

    if (shown.size() != text.size())
    {
      shown = "…" + shown;
    }

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
    {
      break;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
    {
      popUtf8(text);
    }

    else if (ch >= 32 && ch < 256)
    {
      text += (char)ch;
    }
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
  {
    return inputLine("Server address (user@host or user@host:/dir): ");
  }

  items.push_back(manual);
  int hl = 0, top = 0;

  while (true)
  {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int listH = std::max(1, rows - 3);

    if (hl < top)
    {
      top = hl;
    }

    if (hl >= top + listH)
    {
      top = hl - listH + 1;
    }

    erase();
    attron(A_BOLD);
    mvaddstr(0, 0, fitCols("Where to connect? (from ~/.ssh/config)", cols).c_str());
    attroff(A_BOLD);

    for (int r = 0; r < listH && top + r < (int)items.size(); ++r)
    {
      int i = top + r;

      if (i == hl)
      {
        attron(A_REVERSE);
      }

      mvaddstr(r + 2, 0, fitCols("  " + items[i], cols).c_str());

      if (i == hl)
      {
        attroff(A_REVERSE);
      }
    }

    mvaddstr(rows - 1, 0, fitCols("↑↓ select  Enter connect  q quit", cols - 1).c_str());
    refresh();
    int ch = getch();
    int last = (int)items.size() - 1;

    if (ch == KEY_UP && hl > 0)
    {
      --hl;
    }

    else if (ch == KEY_DOWN && hl < last)
    {
      ++hl;
    }

    else if (ch == 'q' || ch == 27 || ch == 3) // q, Esc, Ctrl+C
    {
      return "";
    }

    else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == KEY_RIGHT)
    {
      if (hl != last)
      {
        return items[hl];
      }

      std::string addr = inputLine("Server address (user@host or user@host:/dir): ");

      if (!addr.empty())
      {
        return addr;
      }
    }
  }
}

enum class Protocol
{
  Sftp,
  Ssh
};

static bool pickProtocol(Protocol &mode)
{
  int selected = 0;

  for (;;)
  {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows >= 10 && cols >= 30)
    {
      mvaddstr(1, 2, "Select protocol");
      mvaddstr(3, 2, selected == 0 ? "▶ SFTP" : "  SFTP");
      mvaddstr(4, 2, selected == 1 ? "▶ SSH (scp / rsync)" : "  SSH (scp / rsync)");
      mvaddstr(6, 2,
               fitCols(selected == 0 ? "File access via SFTP; no remote shell required"
                                     : "Shell access via SSH; transfers via scp / rsync",
                       cols - 3)
                   .c_str());
      mvaddstr(rows - 1, 0, fitCols("↑↓ select  Enter confirm  Esc quit", cols - 1).c_str());
    }

    else
    {
      mvaddstr(0, 0, "Window is too small");
    }

    refresh();
    int ch = getch();

    if (ch == 27 || ch == 3 || ch == 'q')
    {
      return false;
    }

    if (ch == KEY_UP)
    {
      selected = 0;
    }

    if (ch == KEY_DOWN)
    {
      selected = 1;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
    {
      mode = selected == 0 ? Protocol::Sftp : Protocol::Ssh;
      return true;
    }
  }
}
