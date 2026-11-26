#pragma once

#include "PaddleOcr.h"

#include <QImage>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <vector>

namespace ws {

// A table read from a picture (screenshots' "复制为表格"), as a spreadsheet
// takes it: rows of cells, each row as wide as the table, and the cells that
// are merged. A merged cell's words are in its top left cell, the rest of it
// is empty.
struct OcrTable {
    struct Span {
        int row = 0;
        int column = 0;
        int rows = 1;
        int columns = 1;
        bool operator==(const Span&) const = default;
    };
    QList<QStringList> rows;
    QList<Span> spans;

    bool isEmpty() const { return rows.isEmpty(); }
    // Words side by side and one above the other: at least two columns with
    // words in two rows each, and two rows with words in two columns each.
    // (The table model makes cells of any picture, a paragraph's lines too.)
    bool standsAsTable() const;
    // Cells with tabs, rows with line breaks (a tab or line break in a cell
    // becomes a space): what any program pastes into cells.
    QString tabbed() const;
    // <table>…</table> with rowspan and colspan: what Excel and WPS prefer,
    // and the only way the merged cells arrive merged. Digits a spreadsheet
    // would change (a leading zero, more than 11 of them) are marked as text.
    QString html() const;
};

namespace table {

// A cell as the structure model says it: where it is in the grid, how many
// rows and columns it takes, and about where its words are in the picture.
struct Cell {
    int row = 0;
    int column = 0;
    int rows = 1;
    int columns = 1;
    QRectF box;
};

struct Structure {
    std::vector<Cell> cells;
    int rows = 0;
    int columns = 0;
    double sure = 0; // how sure the model was of its words, 0…1 (set by who ran it)
};

// The cells from the model's answer: its words ("<tr>", "<td></td>", "<td",
// " colspan=\"2\"", ">", "</td>", …) in order, and a box for every cell
// begun, in the same order. Cells are placed as HTML places them: each in
// the first column of its row that no cell from above reaches into.
Structure structureOf(const QStringList& words, const std::vector<QRectF>& boxes);

// The cells of a ruled table, from the lines drawn in the picture (a
// screenshot's are straight and sharp): the squares between the lines across
// and the lines down, two of them one cell where no line is drawn between
// them. `textHeight`: how tall the writing is, in the picture's pixels (what
// is shorter than a line of the table is a stroke of a character). None when
// the picture has no such ruling: no lines down (a table ruled across only),
// fewer than two rows or columns, or cells that do not come out as rectangles.
Structure ruledStructure(const QImage& picture, double textHeight);

// The lines read put into the cells they stand in: a line by the cell it
// covers most of, or the nearest; a line across several cells of a row (two
// columns close together, read as one line) is cut between them by where
// its characters are. Several lines in a cell are put together top to bottom.
// `tornLines`, when given: how many lines of writing (words side by side)
// ended up in different rows of the table. In a table seen right there are
// none; the model now and then sees a row too many, and tears one.
OcrTable filled(const Structure& structure, const QList<OcrPiece>& pieces, int* tornLines = nullptr);

} // namespace table

} // namespace ws
