#pragma once

#include <panel.h>
#include "fido/window.h"
#include "fido/screen.h"

namespace fido {

class Panel : public Window {
public:
  Panel(Screen *screen, WindowOptions opts);
  Panel(Window *parent, WindowOptions opts);
  ~Panel();

  virtual void Show();
  void Hide();

  void Draw(bool refresh = true) override;

private:
  PANEL* panel_;

};

}
