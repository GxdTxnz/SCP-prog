#pragma once

#include "hosts.hpp"
#include "process.hpp"
#include "sftp_transfer.hpp"

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

// список файлов для файлов которые не были перекинуты полностью
static SftpTransfer::Choice pickResume(const std::string &path, bool canResume)
{
  const char *labels[] = {"Resume", "Start over", "Skip"};
  int first = canResume ? 0 : 1;
  int selected = first;

  for (;;)
  {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows >= 10 && cols >= 30)
    {
      mvaddstr(1, 2, "Incomplete transfer");
      mvaddstr(2, 2, fitCols(path, cols - 3).c_str());

      for (int i = first; i < 3; ++i)
      {
        std::string line = std::string(i == selected ? "▶ " : "  ") + labels[i];
        mvaddstr(4 + i - first, 2, line.c_str());
      }

      if (!canResume)
      {
        mvaddstr(rows - 3, 0, fitCols("Partial copy does not match the source; resume unavailable", cols - 1).c_str());
      }

      mvaddstr(rows - 1, 0, fitCols("Up/Down select  Enter confirm  Esc cancel all", cols - 1).c_str());
    }

    else
    {
      mvaddstr(0, 0, "Window is too small");
    }

    refresh();
    int ch = getch();

    if (ch == 27 || ch == 3 || ch == 'q')
    {
      return SftpTransfer::Choice::Cancel;
    }

    if (ch == KEY_UP && selected > first)
    {
      --selected;
    }

    if (ch == KEY_DOWN && selected < 2)
    {
      ++selected;
    }

    if (rows >= 10 && cols >= 30 && (ch == '\n' || ch == '\r' || ch == KEY_ENTER))
    {
      return selected == 0   ? SftpTransfer::Choice::Resume
             : selected == 1 ? SftpTransfer::Choice::Restart
                             : SftpTransfer::Choice::Skip;
    }
  }
}
