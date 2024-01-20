#pragma once

#include "fido/window.h"

namespace fido {

class TableWindow : public Window {
public:
  TableWindow(Screen *screen, WindowOptions opts,
              const std::vector<std::string> &titles)
      : Window(screen, opts), display_table_(this, titles) {}
  TableWindow(Window *win, WindowOptions opts,
              const std::vector<std::string> &titles)
      : Window(win, opts), display_table_(this, titles) {}
  ~TableWindow() = default;

  void Draw() override {
    Window::Draw();
    display_table_.Draw();
  }

  void Run() override;


protected:
  Table display_table_;

private:
  virtual void RunnerCoroutine(co::Coroutine *c) = 0;
};
} // namespace fido