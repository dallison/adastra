#include "fido/dialog.h"

namespace fido {

YesNoDialog::YesNoDialog(Screen *screen, WindowOptions opts,
                         const std::string &yes, const std::string &no)
    : Panel(screen, opts), yes_(yes), no_(no) {}

YesNoDialog::YesNoDialog(Window *parent, WindowOptions opts,
                         const std::string &yes, const std::string &no)
    : Panel(parent, opts), yes_(yes), no_(no) {}

bool YesNoDialog::GetUserInput(const std::string &prompt, co::Coroutine *c) {
  int yes_col = Width() / 4;
  int no_col = Width() * 3 / 4;
  int prompt_row = 2;
  int button_row = Height() - 2;

  Draw(false);
  PrintAt(prompt_row, (Width() - prompt.size()) / 2, prompt);
  PrintAt(button_row, yes_col, yes_, kColorYesHighlight);
  PrintAt(button_row, no_col, no_, kColorNo);
  Refresh();

  bool yes_selected = true;
  for (;;) {
    c->Wait(STDIN_FILENO, POLLIN);
    int ch = getch();
    switch (ch) {
    case KEY_RIGHT:
    case KEY_LEFT:
    case KEY_UP:
    case KEY_DOWN:
    case '\x09':
      // Swap selected button.
      if (yes_selected) {
        PrintAt(button_row, yes_col, yes_, kColorYes);
        PrintAt(button_row, no_col, no_, kColorNoHighlight);
      } else {
        PrintAt(button_row, yes_col, yes_, kColorYesHighlight);
        PrintAt(button_row, no_col, no_, kColorNo);
      }
      Refresh();
      yes_selected = !yes_selected;
      break;

    case '\033':
      // Escape is the same as selecting no.
      Hide();
      return false;
    case '\n':
    case '\r':
      // Newline or return return selected button.
      Hide();
      return yes_selected;
    }
  }
}

InfoDialog::InfoDialog(Screen *screen, WindowOptions opts,
                       const std::string &ok)
    : Panel(screen, opts), ok_(ok) {}

InfoDialog::InfoDialog(Window *parent, WindowOptions opts,
                       const std::string &ok)
    : Panel(parent, opts), ok_(ok) {}

void InfoDialog::WaitForUser(const std::vector<std::string> &text,
                             co::Coroutine *c) {
  int ok_col = Width() / 2;
  int text_row = 2;
  int button_row = Height() - 2;

  Draw(false);
  for (auto &t : text) {
    PrintAt(text_row, 2, t);
    text_row++;
  }
  PrintAt(button_row, ok_col, ok_, kColorOk);
  Refresh();

  for (;;) {
    c->Wait(STDIN_FILENO, POLLIN);
    int ch = getch();
    switch (ch) {
    case '\033':
    case '\n':
    case '\r':
      Hide();
      return;
    }
  }
}

void HelpWindow::WaitForUser(co::Coroutine *c) {
  return InfoDialog::WaitForUser({"q - quit", "? - help"}, c);
}

} // namespace fido
