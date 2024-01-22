#include "fido/window.h"
#include "absl/strings/str_format.h"
#include "fido/fido.h"

namespace fido {

Window::Window(Screen *screen, WindowOptions opts)
    : opts_(opts), title_(opts.title), screen_(screen), is_subwin_(false) {
  win_ = newwin(opts.nlines, opts.ncols, opts.y, opts.x);
}

Window::Window(Window *parent, WindowOptions opts)
    : opts_(opts), title_(opts.title), parent_(parent), is_subwin_(true) {
  win_ = subwin(parent->win_, opts.nlines, opts.ncols, opts.y, opts.x);
}

Window::~Window() { delwin(win_); }

co::CoroutineScheduler &Window::Scheduler() { return App().Scheduler(); }

void Window::PrintTitle() const {
  int center = Width() / 2;
  int title_length = title_.size() + 2;
  int left = center - title_length / 2;
  wattron(win_, COLOR_PAIR(kColorPairGreen) | A_BOLD);
  mvwprintw(win_, 0, left, " %s ", title_.c_str());
  wattroff(win_, COLOR_PAIR(kColorPairGreen) | A_BOLD);
}

void Window::Draw(bool refresh) {
  if (paused_) {
    return;
  }
  EraseCanvas();
  wborder(win_, 0, 0, 0, 0, 0, 0, 0, 0);
  PrintTitle();
  if (refresh) {
    Refresh();
  }
}

void Window::EraseCanvas() { werase(win_); }

void Window::PrintAt(int row, int col, const std::string &s, int color) {
  ColorOn(color);
  mvwaddstr(win_, row, col, s.c_str());
  ColorOff(color);
}

void Window::PrintInMiddle(int row, const std::string &s, int color) {
  int col = (Width() - s.size()) / 2;
  PrintAt(row, col, s, color);
}

void Window::Move(int row, int col) { wmove(win_, row, col); }

void Window::HLine(int row) {
  wmove(win_, row, 0);
  waddch(win_, ACS_LTEE);
  whline(win_, ACS_HLINE, Width() - 2);
  wmove(win_, row, Width() - 1);
  waddch(win_, ACS_RTEE);
}

void Window::VLine(int col) {
  wmove(win_, 0, col);
  waddch(win_, ACS_TTEE);
  wvline(win_, ACS_VLINE, Height() - 2);
  wmove(win_, Height() - 1, col);
  waddch(win_, ACS_BTEE);
}

} // namespace fido