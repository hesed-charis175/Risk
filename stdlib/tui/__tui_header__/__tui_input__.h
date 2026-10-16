#ifndef TUI_INPUT_H
#define TUI_INPUT_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

void flushStdout(void) { fflush(stdout); }

int kbHit(void) {
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static struct termios tui_input_saved_termios;
static int tui_input_raw_active = 0;

void rawModeEnter(void) {
  if (tui_input_raw_active)
    return;
  setvbuf(stdout, NULL, _IONBF, 0);
  tcgetattr(STDIN_FILENO, &tui_input_saved_termios);
  struct termios raw = tui_input_saved_termios;
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  tui_input_raw_active = 1;
}

void rawModeLeave(void) {
  if (!tui_input_raw_active)
    return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &tui_input_saved_termios);
  tui_input_raw_active = 0;
}

static inline int tui_input_read_byte(int timeout_us) {
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = timeout_us;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  int sr =
      select(STDIN_FILENO + 1, &fds, NULL, NULL, timeout_us > 0 ? &tv : NULL);
  if (sr <= 0)
    return -1;
  unsigned char c;
  return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

int kbReadKey(void) {
  unsigned char first;
  while (read(STDIN_FILENO, &first, 1) != 1) {
  }
  int c1 = (int)first;

  if (c1 != 0x1b) {
    return c1;
  }
  int c2 = tui_input_read_byte(50000);
  if (c2 < 0)
    return 27;
  if (c2 != '[' && c2 != 'O')
    return 27;

  int c3 = tui_input_read_byte(50000);
  if (c3 < 0)
    return 27;

  if (c2 == 'O') {
    switch (c3) {
    case 'A':
      return 1000;
    case 'B':
      return 1001;
    case 'C':
      return 1002;
    case 'D':
      return 1003;
    case 'H':
      return 1004;
    case 'F':
      return 1005;
    case 'P':
      return 1011;
    case 'Q':
      return 1012;
    case 'R':
      return 1013;
    case 'S':
      return 1014;
    default:
      return 27;
    }
  }

  if ((c3 >= 'A' && c3 <= 'Z') || c3 == 'a' || c3 == 'b') {
    switch (c3) {
    case 'A':
      return 1000;
    case 'B':
      return 1001;
    case 'C':
      return 1002;
    case 'D':
      return 1003;
    case 'H':
      return 1004;
    case 'F':
      return 1005;
    default:
      return 27;
    }
  }

  int num1 = 0, num2 = 0;
  int cn = c3;
  while (cn >= '0' && cn <= '9') {
    num1 = num1 * 10 + (cn - '0');
    cn = tui_input_read_byte(50000);
    if (cn < 0)
      return 27;
  }

  if (cn == '~') {
    switch (num1) {
    case 1:
      return 1004;
    case 2:
      return 1006;
    case 3:
      return 1007;
    case 4:
      return 1005;
    case 5:
      return 1008;
    case 6:
      return 1009;
    case 11:
      return 1011;
    case 12:
      return 1012;
    case 13:
      return 1013;
    case 14:
      return 1014;
    case 15:
      return 1015;
    case 17:
      return 1016;
    case 18:
      return 1017;
    case 19:
      return 1018;
    case 20:
      return 1019;
    case 21:
      return 1020;
    case 23:
      return 1021;
    case 24:
      return 1022;
    default:
      return 27;
    }
  }

  if (cn == ';') {
    int mc = tui_input_read_byte(50000);
    while (mc >= '0' && mc <= '9') {
      num2 = num2 * 10 + (mc - '0');
      mc = tui_input_read_byte(50000);
      if (mc < 0)
        return 27;
    }
    if (num1 == 1) {
      int base = 0;
      switch (mc) {
      case 'A':
        base = 1000;
        break;
      case 'B':
        base = 1001;
        break;
      case 'C':
        base = 1002;
        break;
      case 'D':
        base = 1003;
        break;
      default:
        return 27;
      }
      if (num2 == 2)
        return base + 30;
      if (num2 == 5)
        return base + 40;
      if (num2 == 3)
        return base + 50;
      return base;
    }
  }

  return 27;
}

#ifdef __cplusplus
}
#endif

#endif
