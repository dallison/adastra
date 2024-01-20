#pragma once

#include "fido/screen.h"
#include <string>
#include <vector>

namespace fido {

struct TableData {

  struct Cell {
    std::string data;
    int color; // ColorPair number.
  };

  struct Column {
    std::vector<Cell> cells;
  };

  std::vector<Column> cols;
};

class Window;

class Table {
public:
  struct Cell {
    std::string data;
    int color; // ColorPair number.
  };

  Table(Window *win, const std::vector<std::string> titles,
        ssize_t sort_column = 0,
        std::function<bool(const std::string &, const std::string &)> comp =
            nullptr);
  ~Table();

  void AddRow(const std::vector<std::string> cells);
  void AddRow(const std::vector<std::string> cells, int color);
  void AddRowWithColors(const std::vector<Cell> cells);
  void AddRow();
  void SetCell(size_t col, Cell &&cell);

  void Fill(const TableData &data);

  void Draw(int start_row = 0);
  void Clear();

  // Sort data using the comparison function, which must correspond to that
  // needed by std::sort.
  void
  SortBy(size_t column,
         std::function<bool(const std::string &, const std::string &)> comp) {
    sort_column_ = column;
    if (comp == nullptr) {
      // Sort by string.
      sorter_ = [](const std::string &a, const std::string &b) -> bool {
        return a < b;
      };
    } else {
      sorter_ = std::move(comp);
    }
  }

  // Sort data using the column.  Comparison is done by string comparison.
  void SortBy(size_t column) { SortBy(column, nullptr); }

  static Cell MakeCell(std::string data, int color = kColorPairNormal) {
    return Cell({.data = std::move(data), .color = color});
  }

  void SetWrapColumn(size_t col, int width) {
    wrap_column_ = col;
    wrap_column_width_ = width;
  }

private:
  struct Column {
    std::string title;
    int width;
    std::vector<Cell> cells;
  };

  void Render(int width);
  void Sort();

  void AddCell(size_t col, const Cell &cell);

  Window *win_;
  std::vector<Column> cols_;
  int num_rows_ = 0;

  size_t sort_column_;
  size_t wrap_column_ = -1;
  int wrap_column_width_ = 0;
  std::function<bool(const std::string &, const std::string &)> sorter_;
};

} // namespace fido