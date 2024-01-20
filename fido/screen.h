#pragma once

#include <ncurses.h>
#include <string>

namespace fido {

class Application;

// Fixed color pairs.
constexpr int kColorPairNormal = 0;

constexpr int kColorPairBlack = 1;
constexpr int kColorPairRed = 2;
constexpr int kColorPairGreen = 3;
constexpr int kColorPairYellow = 4;
constexpr int kColorPairBlue = 5;
constexpr int kColorPairMagenta = 6;
constexpr int kColorPairCyan = 7;
constexpr int kColorPairWhite = 8;

class Screen {
public:
  Screen(Application& app);
  ~Screen();

  void Open();
  void Close();

  void PrintAt(int row, int col, const std::string &s, int color = kColorPairNormal);
  void PrintInMiddle(int row, const std::string &s, int color = kColorPairNormal);

  int Width() const;
  int Height() const;

  Application& App() const { return app_; }

void ColorOn(int color) {
    if (color != kColorPairNormal) {
      attron(COLOR_PAIR(color) | A_BOLD);
    } else {
      attron(A_BOLD);
    }
  }

 void ColorOff(int color) {
    if (color != kColorPairNormal) {
      attroff(COLOR_PAIR(color) | A_BOLD);
   } else {
      attroff(A_BOLD);
    }
  }
  
private:
  friend class Window;
  Application& app_;
  WINDOW *win_;
  bool is_open_ = false;
};
} // namespace fido