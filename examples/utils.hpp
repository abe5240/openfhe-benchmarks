// examples/utils.hpp - Shared utilities for benchmarks
#ifndef OPENFHE_BENCHMARKS_UTILS_HPP
#define OPENFHE_BENCHMARKS_UTILS_HPP

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <cassert>
#include <unordered_map>

// -----------------------------------------------------------------------------
// Matrix<T>: row-major r×c container, supports M[i][j] access
// -----------------------------------------------------------------------------
template<typename T>
struct Matrix {
    std::size_t rows, cols;
    std::vector<T> data;
    Matrix(std::size_t r, std::size_t c)
      : rows(r), cols(c), data(r*c) {}
    T*       operator[](std::size_t i)       { assert(i < rows); return data.data() + i*cols; }
    const T* operator[](std::size_t i) const { assert(i < rows); return data.data() + i*cols; }
};

// -----------------------------------------------------------------------------
// Embed a dim×dim random block into the top-left of an n×n zero matrix
// -----------------------------------------------------------------------------
inline Matrix<double> make_embedded_random_matrix(std::size_t dim, std::size_t n) {
    assert(dim <= n);
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    Matrix<double> M(n, n);
    // zero-initialize via constructor; overwrite block
    for (std::size_t i = 0; i < dim; ++i)
        for (std::size_t j = 0; j < dim; ++j)
            M[i][j] = dist(gen);
    return M;
}

// -----------------------------------------------------------------------------
// Extract all non-empty diagonals from a matrix
// -----------------------------------------------------------------------------
inline std::unordered_map<int, std::vector<double>>
extract_generalized_diagonals(const Matrix<double>& M, std::size_t dim) {
    const std::size_t n = M.rows;  // Total number of slots
    
    std::unordered_map<int, std::vector<double>> diagonals;
    
    // For each possible diagonal k
    for (std::size_t k = 0; k < n; ++k) {
        std::vector<double> diagonal(n, 0.0);
        bool has_data = false;
        
        // Extract diagonal k using cyclic indexing
        // Diagonal k contains elements where col = (row + k) % n
        for (std::size_t row = 0; row < dim; ++row) {
            std::size_t col = (row + k) % n;
            
            // Only extract if column is within the actual data region
            if (col < dim) {
                diagonal[row] = M[row][col];
                if (M[row][col] != 0.0) {
                    has_data = true;
                }
            }
        }
        // Only store non-empty diagonals
        if (has_data) {
            diagonals[k] = diagonal;
        }
    }
    return diagonals;
}

// -----------------------------------------------------------------------------
// Matrix-vector multiplication for verification (dim×dim block only)
// -----------------------------------------------------------------------------
inline std::vector<double> matrix_vector_multiply(const Matrix<double>& M, 
                                                  const std::vector<double>& v, 
                                                  std::size_t dim) {
    std::vector<double> result(dim, 0.0);
    for (std::size_t i = 0; i < dim; ++i) {
        for (std::size_t j = 0; j < dim; ++j) {
            result[i] += M[i][j] * v[j];
        }
    }
    return result;
}

// -----------------------------------------------------------------------------
// Print matrix for debugging
// -----------------------------------------------------------------------------
inline void print_matrix(const Matrix<double>& M, std::size_t dim) {
    std::cout << "Matrix (showing " << dim << "×" << dim << " block):\n";
    for (std::size_t i = 0; i < dim && i < 5; ++i) {
        for (std::size_t j = 0; j < dim && j < 5; ++j) {
            std::cout << std::setw(8) << std::fixed 
                     << std::setprecision(4) << M[i][j];
        }
        if (dim > 5) std::cout << " ...";
        std::cout << '\n';
    }
    if (dim > 5) std::cout << "...\n";
}

// -----------------------------------------------------------------------------
// Verify FHE matrix-vector multiplication result
// -----------------------------------------------------------------------------
inline void verify_matrix_vector_result(
    const std::vector<double>& fheResult,
    const Matrix<double>& M,
    const std::vector<double>& inputVec,
    std::size_t matrixDim) {
    
    // Compute expected result (only first matrixDim elements)
    std::vector<double> inputOriginal(inputVec.begin(), inputVec.begin() + matrixDim);
    auto expected = matrix_vector_multiply(M, inputOriginal, matrixDim);
    
    // Compare results
    std::cout << "\nResults (first " << std::min(matrixDim, std::size_t(10)) << " elements):\n";
    std::cout << "FHE:      ";
    for (std::size_t i = 0; i < matrixDim && i < 10; ++i) {
        std::cout << std::setw(8) << std::fixed << std::setprecision(4) << fheResult[i];
    }
    if (matrixDim > 10) std::cout << " ...";
    std::cout << "\nExpected: ";
    for (std::size_t i = 0; i < matrixDim && i < 10; ++i) {
        std::cout << std::setw(8) << std::fixed << std::setprecision(4) << expected[i];
    }
    if (matrixDim > 10) std::cout << " ...";
    std::cout << "\n";
    
    // Calculate error metrics
    double maxError = 0.0;
    double avgError = 0.0;
    for (std::size_t i = 0; i < matrixDim; ++i) {
        double error = std::abs(fheResult[i] - expected[i]);
        maxError = std::max(maxError, error);
        avgError += error;
    }
    avgError /= matrixDim;
    
    std::cout << "\nMax error: " << std::scientific << maxError << "\n";
    std::cout << "Avg error: " << std::scientific << avgError << "\n";
}

#endif // OPENFHE_BENCHMARKS_UTILS_HPP