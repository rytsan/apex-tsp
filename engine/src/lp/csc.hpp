#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace apex {

enum class Sense : uint8_t { Ge, Le, Eq };

struct Entry {
    uint32_t index;  // column index inside a Row; row index inside a column
    double value;
};

struct Row {
    std::vector<Entry> entries;  // (column, coefficient)
    Sense sense = Sense::Ge;
    double rhs = 0;
};

struct RowData {
    Sense sense;
    double rhs;
};

struct Triplet {
    uint32_t row, col;
    double value;
};

// Column-major sparse matrix with a row-wise append buffer.
// - Appended rows land in a buffer and are merged into CSC once the buffer
//   exceeds max_buffered_rows (or on ensure_merged()).
// - Column views require the merged state (call ensure_merged() first).
// - A CSR (row-wise) copy is kept in sync for row access and transposition.
// Duplicate coordinates are summed; exact zeros are dropped.
class CscMatrix {
public:
    explicit CscMatrix(uint32_t rows = 0, uint32_t cols = 0, uint32_t max_buffered_rows = 4096);

    static CscMatrix from_triplets(uint32_t rows, uint32_t cols, std::vector<Triplet> t,
                                   std::vector<RowData> row_data = {});

    // Appends column j (must equal num_cols()); entries are (row, value).
    void add_column(uint32_t j, std::vector<Entry> entries);
    void append_rows(const std::vector<Row>& rows);
    void ensure_merged();

    uint32_t num_rows() const { return static_cast<uint32_t>(row_data_.size()); }
    uint32_t num_cols() const { return static_cast<uint32_t>(col_start_.size() - 1); }
    size_t nnz() const { return csr_idx_.size(); }
    uint32_t pending_rows() const { return pending_; }

    std::span<const uint32_t> col_rows(uint32_t j) const;   // throws if rows are pending
    std::span<const double> col_vals(uint32_t j) const;
    std::span<const uint32_t> row_cols(uint32_t i) const;   // always valid
    std::span<const double> row_vals(uint32_t i) const;

    const std::vector<RowData>& row_data() const { return row_data_; }
    std::vector<RowData>& row_data() { return row_data_; }

    // Transpose (row data is dropped: rows of A^T are columns of A).
    CscMatrix transposed() const;

    bool operator==(const CscMatrix& o) const;

private:
    void rebuild_csr();
    void check_merged() const;

    uint32_t max_buffered_;
    uint32_t pending_ = 0;                 // rows present in CSR but not yet in CSC
    std::vector<uint32_t> col_start_{0};   // CSC
    std::vector<uint32_t> row_idx_;
    std::vector<double> val_;
    std::vector<uint32_t> csr_start_{0};   // CSR (always complete)
    std::vector<uint32_t> csr_idx_;
    std::vector<double> csr_val_;
    std::vector<RowData> row_data_;
};

}  // namespace apex
