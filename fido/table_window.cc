#include "fido/table_window.h"
#include "fido/fido.h"

namespace fido {

void TableWindow::Run() {
  Draw();
  App().AddCoroutine(std::make_unique<co::Coroutine>(
      Scheduler(), [this](co::Coroutine *c) { RunnerCoroutine(c); }));
}

} // namespace fido
