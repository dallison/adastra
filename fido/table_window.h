#pragma once

#include "fido/panel.h"

namespace fido {

class TableWindow : public Panel {
public:
  TableWindow(Screen *screen, WindowOptions opts,
              const std::vector<std::string> &titles)
      : Panel(screen, opts), display_table_(this, titles) {}
  TableWindow(Window *win, WindowOptions opts,
              const std::vector<std::string> &titles)
      : Panel(win, opts), display_table_(this, titles) {}
  ~TableWindow() = default;

  void Draw(bool refresh = true) override {
    if (paused_) {
      return;
    }
    Panel::Draw(refresh);
    display_table_.Draw();
  }

  void Run() override;


protected:
  Table display_table_;

private:
  virtual void RunnerCoroutine(co::Coroutine *c) = 0;
};
} // namespace fido