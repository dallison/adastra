#include "fido/panel.h"

namespace fido {

Panel::Panel(Screen *screen, WindowOptions opts)
    : Window(screen, std::move(opts)) {
  panel_ = new_panel(win_);
}

Panel::Panel(Window *parent, WindowOptions opts)
    : Window(parent, std::move(opts)) {
  panel_ = new_panel(win_);
}

Panel::~Panel() { del_panel(panel_); }

void Panel::Draw(bool refresh) {
  if (paused_) {
    return;
  }
  Window::Draw(false);
  show_panel(panel_);
  update_panels();
  if (refresh) {
    doupdate();
  }
}

void Panel::Show() {
  Window::Draw(false);
  top_panel(panel_);
  update_panels();
  show_panel(panel_);
  doupdate();
}

void Panel::Hide() {
  update_panels();
  hide_panel(panel_);
  doupdate();
}
} // namespace fido
