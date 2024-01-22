#pragma once

#include "fido/screen.h"
#include "toolbelt/pipe.h"
#include "fido/table.h"
#include "coroutine.h"

#include <string>

namespace fido {

class Application;

struct WindowOptions {
  std::string title;
  int nlines;
  int ncols;
  int y;
  int x;
};

class Window {
public:
  Window(Screen *screen, WindowOptions opts);
  Window(Window *parent, WindowOptions opts);
  virtual ~Window();

  virtual void Draw(bool refresh = true);
  virtual void Run() {}

  void EraseCanvas();

  void Refresh() const { wrefresh(win_);}

  int Width() const { return opts_.ncols; }
  int Height() const { return opts_.nlines; }

  void PrintAt(int row, int col, const std::string &s, int color = kColorPairNormal);
  void PrintInMiddle(int row, const std::string &s, int color = kColorPairNormal);
  void Print(const std::string& s) {
    waddstr(win_, s.c_str());
  }

  void Move(int row, int col);

  void HLine(int row);
  void VLine(int col);

  void ColorOn(int color) {
    if (color != kColorPairNormal) {
      wattron(win_, COLOR_PAIR(color) | A_BOLD);
    } else {
      wattron(win_, A_BOLD);
    }
  }

 void ColorOff(int color) {
    if (color != kColorPairNormal) {
      wattroff(win_, COLOR_PAIR(color) | A_BOLD);
   } else {
      wattroff(win_, A_BOLD);
    }
  }

  Application& App() const {
    if (is_subwin_) {
      return parent_->App();
    }
    return screen_->App();
  }

  co::CoroutineScheduler& Scheduler();

  void Pause() { paused_ = true; }
  void Resume() { paused_ = false; }
  
protected:
  friend class Panel;
  void PrintTitle() const;

  Application *app_;
  WINDOW *win_;
  WindowOptions opts_;
  std::string title_;
  union {
    Screen *screen_;
    Window *parent_;
  };
  bool is_subwin_;
  bool paused_ = false;
};
} // namespace fido