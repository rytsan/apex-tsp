#include "lp/csc.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace apex {
namespace {

// Sort by index, sum duplicates, drop exact zeros.
void normalize(std::vector<Entry>& e) {
    std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.index < b.index; });
    size_t w = 0;
    for (size_t r = 0; r < e.size();) {
        Entry acc = e[r++];
        while (r < e.size() && e[r].index == acc.index) acc.value += e[r++].value;
        if (acc.value != 0.0) e[w++] = acc;
    }
    e.resize(w);
}

}  // namespace

CscMatrix::CscMatrix(uint32_t rows, uint32_t cols, uint32_t max_buffered_rows)
    : max_buffered_(max_buffered_rows), col_start_(cols + 1, 0), csr_start_(rows + 1, 0),
      row_data_(rows, RowData{Sense::Ge, 0.0}) {}

CscMatrix CscMatrix::from_triplets(uint32_t rows, uint32_t cols, std::vector<Triplet> t,
                                   std::vector<RowData> row_data) {
    CscMatrix m(rows, cols);
    if (!row_data.empty()) {
        if (row_data.size() != rows) throw std::invalid_argument("from_triplets: row_data size mismatch");
        m.row_data_ = std::move(row_data);
    }
    for (const auto& x : t)
        if (x.row >= rows || x.col >= cols) throw std::out_of_range("from_triplets: index out of range");
    std::sort(t.begin(), t.end(), [](const Triplet& a, const Triplet& b) {
        return a.col != b.col ? a.col < b.col : a.row < b.row;
    });
    for (size_t r = 0; r < t.size();) {
        Triplet acc = t[r++];
        while (r < t.size() && t[r].col == acc.col && t[r].row == acc.row) acc.value += t[r++].value;
        if (acc.value == 0.0) continue;
        m.row_idx_.push_back(acc.row);
        m.val_.push_back(acc.value);
        ++m.col_start_[acc.col + 1];
    }
    for (uint32_t j = 0; j < cols; ++j) m.col_start_[j + 1] += m.col_start_[j];
    m.rebuild_csr();
    return m;
}

void CscMatrix::add_column(uint32_t j, std::vector<Entry> entries) {
    if (j != num_cols()) throw std::invalid_argument("add_column: j must equal num_cols()");
    ensure_merged();
    normalize(entries);
    for (const auto& e : entries) {
        if (e.index >= num_rows()) throw std::out_of_range("add_column: row index out of range");
        row_idx_.push_back(e.index);
        val_.push_back(e.value);
    }
    col_start_.push_back(static_cast<uint32_t>(row_idx_.size()));
    rebuild_csr();  // column insert is O(nnz) in CSR; columns are added rarely (pricing)
}

void CscMatrix::append_rows(const std::vector<Row>& rows) {
    for (const auto& r : rows) {
        std::vector<Entry> e = r.entries;
        normalize(e);
        for (const auto& x : e) {
            if (x.index >= num_cols()) throw std::out_of_range("append_rows: column index out of range");
            csr_idx_.push_back(x.index);
            csr_val_.push_back(x.value);
        }
        csr_start_.push_back(static_cast<uint32_t>(csr_idx_.size()));
        row_data_.push_back(RowData{r.sense, r.rhs});
        ++pending_;
    }
    if (pending_ > max_buffered_) ensure_merged();
}

void CscMatrix::ensure_merged() {
    if (pending_ == 0) return;
    const uint32_t first = num_rows() - pending_, cols = num_cols();
    std::vector<uint32_t> add(cols + 1, 0);
    for (uint32_t i = first; i < num_rows(); ++i)
        for (uint32_t p = csr_start_[i]; p < csr_start_[i + 1]; ++p) ++add[csr_idx_[p] + 1];
    std::vector<uint32_t> start(cols + 1, 0);
    for (uint32_t j = 0; j < cols; ++j)
        start[j + 1] = start[j] + (col_start_[j + 1] - col_start_[j]) + add[j + 1];
    std::vector<uint32_t> idx(start[cols]);
    std::vector<double> val(start[cols]);
    std::vector<uint32_t> fill(cols);
    for (uint32_t j = 0; j < cols; ++j) {  // old entries keep their (sorted) order...
        uint32_t w = start[j];
        for (uint32_t p = col_start_[j]; p < col_start_[j + 1]; ++p, ++w) {
            idx[w] = row_idx_[p];
            val[w] = val_[p];
        }
        fill[j] = w;
    }
    for (uint32_t i = first; i < num_rows(); ++i)  // ...new rows have larger indices: still sorted
        for (uint32_t p = csr_start_[i]; p < csr_start_[i + 1]; ++p) {
            const uint32_t j = csr_idx_[p];
            idx[fill[j]] = i;
            val[fill[j]++] = csr_val_[p];
        }
    col_start_ = std::move(start);
    row_idx_ = std::move(idx);
    val_ = std::move(val);
    pending_ = 0;
}

void CscMatrix::rebuild_csr() {
    const uint32_t rows = num_rows(), cols = num_cols();
    csr_start_.assign(rows + 1, 0);
    for (uint32_t r : row_idx_) ++csr_start_[r + 1];
    for (uint32_t i = 0; i < rows; ++i) csr_start_[i + 1] += csr_start_[i];
    csr_idx_.resize(row_idx_.size());
    csr_val_.resize(row_idx_.size());
    std::vector<uint32_t> fill(csr_start_.begin(), csr_start_.end() - 1);
    for (uint32_t j = 0; j < cols; ++j)
        for (uint32_t p = col_start_[j]; p < col_start_[j + 1]; ++p) {
            const uint32_t i = row_idx_[p];
            csr_idx_[fill[i]] = j;
            csr_val_[fill[i]++] = val_[p];
        }
}

void CscMatrix::check_merged() const {
    if (pending_ != 0) throw std::logic_error("CscMatrix: column access with " + std::to_string(pending_) +
                                              " pending rows (call ensure_merged())");
}

std::span<const uint32_t> CscMatrix::col_rows(uint32_t j) const {
    check_merged();
    return {row_idx_.data() + col_start_[j], col_start_[j + 1] - col_start_[j]};
}

std::span<const double> CscMatrix::col_vals(uint32_t j) const {
    check_merged();
    return {val_.data() + col_start_[j], col_start_[j + 1] - col_start_[j]};
}

std::span<const uint32_t> CscMatrix::row_cols(uint32_t i) const {
    return {csr_idx_.data() + csr_start_[i], csr_start_[i + 1] - csr_start_[i]};
}

std::span<const double> CscMatrix::row_vals(uint32_t i) const {
    return {csr_val_.data() + csr_start_[i], csr_start_[i + 1] - csr_start_[i]};
}

CscMatrix CscMatrix::transposed() const {
    // CSR of A is exactly CSC of A^T, and CSC of A is CSR of A^T.
    CscMatrix t(0, 0, max_buffered_);
    t.col_start_ = csr_start_;
    t.row_idx_ = csr_idx_;
    t.val_ = csr_val_;
    t.row_data_.assign(num_cols(), RowData{Sense::Ge, 0.0});
    t.rebuild_csr();
    return t;
}

bool CscMatrix::operator==(const CscMatrix& o) const {
    return pending_ == 0 && o.pending_ == 0 && col_start_ == o.col_start_ && row_idx_ == o.row_idx_ &&
           val_ == o.val_ && num_rows() == o.num_rows();
}

}  // namespace apex
