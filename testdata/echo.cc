#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  std::string line;
  std::cout << "running" << std::endl;
  while (std::getline(std::cin, line)) {
    std::cout << line << std::endl;
  }
  std::cout << "done" << std::endl;
}