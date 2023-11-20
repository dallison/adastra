// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/capcom.h"
#include "coroutine.h"

#include <iostream>

int main(int argc, char** argv) {
  co::CoroutineScheduler scheduler;
  scheduler.Run();
}