#pragma once

#include "fido/panel.h"

namespace fido {

class YesNoDialog : public Panel {
public:
  YesNoDialog(Screen *screen, WindowOptions opts, const std::string &yes,
              const std::string &no);
  YesNoDialog(Window *parent, WindowOptions opts, const std::string &yes,
              const std::string &no);

  bool GetUserInput(const std::string &prompt, co::Coroutine *c);

private:
  std::string yes_;
  std::string no_;
};

class InfoDialog : public Panel {
public:
  InfoDialog(Screen *screen, WindowOptions opts, const std::string &ok);
  InfoDialog(Window *parent, WindowOptions opts, const std::string &ok);

  void WaitForUser(const std::vector<std::string> &text, co::Coroutine *c);

private:
  std::string ok_;
};

class HelpWindow : public InfoDialog {
public:
  HelpWindow(Screen *screen, const std::string &ok)
      : InfoDialog(screen,
                   WindowOptions{.title = "Help",
                                 .nlines = 7,
                                 .ncols = 30,
                                 .x = screen->Width() / 2 - 15,
                                 .y = screen->Height() / 2 - 3},
                   ok) {}
  HelpWindow(Window *parent, const std::string &ok)
      : InfoDialog(parent,
                   WindowOptions{.title = "Quit",
                                 .nlines = 7,
                                 .ncols = 30,
                                 .x = parent->Width() / 2 - 15,
                                 .y = parent->Height() / 2 - 3},
                   ok) {}

  void WaitForUser(co::Coroutine *c);
};
} // namespace fido
