#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <termios.h>

int main(int argc, char **argv) {
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  std::string line;
  std::cout << "running" << std::endl;
  system("stty -a");
  for (;;) {
    char buf[1];
    ssize_t n = ::read(0, buf, 1);
    if (n <= 0) {
      break;
    }
    char hex[16];
    int x = snprintf(hex, sizeof(hex), "%02x", buf[0] & 0xff);
    ::write(1, hex, x);
    // ::write(1, buf, 1);
  }
  std::cout << "done" << std::endl;
}
