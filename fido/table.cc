// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/table.h"
#include "absl/strings/str_format.h"
#include "fido/window.h"

namespace fido {

Table::Table(Window *win, const std::vector<std::string> titles,
             ssize_t sort_column,
             std::function<bool(const std::string &, const std::string &)> comp)
    : win_(win) {
  SortBy(sort_column, comp);
  for (auto &title : titles) {
    cols_.push_back({.title = title});
  }
}

Table::~Table() {}

void Table::AddRow() {
  for (auto &col : cols_) {
    col.cells.push_back({});
  }
  ++num_rows_;
}

void Table::AddRow(const std::vector<std::string> cells) {
  AddRow(cells, kColorPairNormal);
}

void Table::SetCell(size_t col, Cell &&cell) {
  cols_[col].cells[num_rows_ - 1] = std::move(cell);
}

void Table::AddRow(const std::vector<std::string> cells, int color) {
  size_t index = 0;
  for (auto &cell : cells) {
    if (index >= cols_.size()) {
      break;
    }
    AddCell(index, {.data = cell, .color = color});
    index++;
  }
  ++num_rows_;
}

void Table::AddRowWithColors(const std::vector<Cell> cells) {
  size_t index = 0;
  for (auto &cell : cells) {
    if (index >= cols_.size()) {
      break;
    }
    AddCell(index, cell);
    index++;
  }
  ++num_rows_;
}

void Table::AddCell(size_t col, const Cell &cell) {
  cols_[col].cells.push_back(cell);
}

void Table::Fill(const TableData &data) {
  Clear();
  for (auto &col : data.cols) {
    std::vector<Cell> cells;
    cells.reserve(col.cells.size());
    for (auto &cell : col.cells) {
      cells.push_back({.data = cell.data, .color = cell.color});
    }
    AddRowWithColors(cells);
  }
}

void Table::Draw(int start_row) {
  int width = win_->Width();

  // Calculate the widths for each column.
  Render(width);

  // Print titles.
  int x = 1;
  for (auto &col : cols_) {
    std::string title = col.title;
    if (title.size() > col.width) {
      title = title.substr(0, col.width - 1);
    }
    win_->PrintAt(1, x, title);
    x += col.width;
  }
  // Print separator line.
  win_->HLine(2);

  if (num_rows_ == 0) {
    win_->PrintInMiddle(win_->Height() / 2, "NO SIGNAL", kColorPairRed);
    win_->Refresh();
    return;
  }

  // Print each row.
  int y = 3;
  for (size_t i = start_row; i < num_rows_; i++) {
    int x = 1;
    int col_index = 0;
    for (auto &col : cols_) {
      std::string data = col.cells[i].data;
      if (wrap_column_ == col_index && data.size() > col.width) {
        // Need to wrap the column.
        // To do this, we split the data at good locations (before spaces)
        // and place the segments in additional rows at the same
        // column.
        size_t start = 0;
        for (;;) {
          std::string segment = data.substr(start);
          if (segment.size() > col.width) {
            segment = segment.substr(0, col.width);
            // Move back to the first space to avoid splitting words.
            ssize_t i = segment.size() - 1;
            while (i > 0) {
              if (isspace(segment[i])) {
                break;
              }
              i--;
            }
            // If there is no space we just split the word.
            if (i != 0) {
              segment = segment.substr(0, i);
            }
          }
          // Draw segment at the column location.
          win_->PrintAt(y, x, segment, col.cells[i].color);

          // Move to end of segment.
          start += segment.size();

          // Skip spaces for next row.
          while (start < data.size() && isspace(data[start])) {
            start++;
          }
          if (start >= data.size()) {
            break;
          }
          if (y > win_->Height() - 1) {
            break;
          }
          // Move on to the next row for the next segment.
          y++;
        }
      } else {
        if (data.size() > col.width) {
          // Truncate if too wide.
          data = data.substr(0, col.width - 1);
        }
        win_->PrintAt(y, x, data, col.cells[i].color);
      }
      x += col.width;
      col_index++;
    }
    if (y > win_->Height() - 1) {
      break;
    }
    y++;
  }
  win_->Refresh();
}

void Table::Clear() {
  for (auto &col : cols_) {
    col.cells.clear();
  }
  num_rows_ = 0;
}

void Table::Render(int width) {
  std::vector<size_t> max_widths(cols_.size());
  for (size_t i = 0; i < num_rows_; i++) {
    int col_index = 0;
    for (auto &col : cols_) {
      if (col.cells[i].data.size() > max_widths[col_index]) {
        max_widths[col_index] = col.cells[i].data.size();
      }
      col_index++;
    }
  }
  // If we have a wrap column and its width is greater than the
  // width specified, use the width specified.  The actual wrapping
  // will occur when the table is drawn.
  if (wrap_column_ != -1) {
    if (max_widths[wrap_column_] > wrap_column_width_) {
      max_widths[wrap_column_] = wrap_column_width_;
    }
  }
  size_t total_width = 0;
  for (size_t w : max_widths) {
    total_width += w;
  }
  // Pad the column widths out to the width we have.
  ssize_t padding = width - total_width;
  if (padding < 0) {
    padding = 0;
  } else {
    padding /= cols_.size();
  }
  int index = 0;
  for (auto &col : cols_) {
    col.width = max_widths[index] + padding;
    index++;
  }
  Sort();
}

void Table::Sort() {
  if (sort_column_ == -1 || sort_column_ >= cols_.size()) {
    return;
  }
  struct Index {
    size_t row;
    std::string data;
  };
  std::vector<Index> index(num_rows_);
  for (size_t i = 0; i < num_rows_; i++) {
    index[i] = {.row = i, .data = cols_[sort_column_].cells[i].data};
  }
  std::sort(index.begin(), index.end(), [this](const Index &a, const Index &b) {
    return sorter_(a.data, b.data);
  });
  // Now we have the index in the correct order, reorder the table cells.
  // We do this my moving all the cells into a new vector of columns in
  // the order specified by the index.  We then use the sorted vector
  // as the new columns in the table.
  std::vector<Column> sorted;
  sorted.reserve(cols_.size());
  for (auto &col : cols_) {
    sorted.push_back({.title = col.title, .width = col.width});
  }
  for (auto &i : index) {
    // i.row contains the row number for the next row to be inserted
    // into the new sorted table.
    for (size_t col = 0; col < cols_.size(); col++) {
      sorted[col].cells.push_back(std::move(cols_[col].cells[i.row]));
    }
  }
  cols_ = std::move(sorted);
}

} // namespace fido