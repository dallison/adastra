#include "stagezero/capcom/capcom.h"
#include "coroutine.h"

#include <iostream>

int main(int argc, char** argv) {
  co::CoroutineScheduler scheduler;
  scheduler.Run();
}