// -----------------------------------------------------------------------------
// Create a random input vector with values in [-1, 1]
// -----------------------------------------------------------------------------
inline std::vector<double> make_random_input_vector(std::size_t dim, std::size_t numSlots, 
                                                   bool print = true,
                                                   std::size_t maxElementsToShow = 10) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    
    // Create vector with zeros, fill first dim elements with random values
    std::vector<double> inputVec(numSlots, 0.0);
    for (std::size_t i = 0; i < dim; ++i) {
        inputVec[i] = dist(gen);
    }
    
    // Optionally print the vector
    if (print) {
        std::size_t elementsToShow = std::min(dim, maxElementsToShow);
        std::cout << "\nInput vector (first " << elementsToShow << " elements): ";
        std::cout << std::fixed << std::setprecision(3);
        for (std::size_t i = 0; i < elementsToShow; ++i) {
            std::cout << std::setw(7) << inputVec[i] << " ";
        }
        if (dim > elementsToShow) std::cout << "...";
        std::cout << "\n\n";
    }
    
    return inputVec;
}// examples/utils.hpp - Shared utilities for benchmarks
#ifndef OPENFHE_BENCHMARKS_UTILS_HPP
#define OPENFHE_BENCHMARKS_UTILS_HPP

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <cassert>
#include <map>

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
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

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
inline std::map<int, std::vector<double>>
extract_generalized_diagonals(const Matrix<double>& M, std::size_t dim) {
    const std::size_t n = M.rows;  // Total number of slots
    
    std::map<int, std::vector<double>> diagonals;
    
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
            diagonals[static_cast<int>(k)] = diagonal;
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
inline void print_matrix(const Matrix<double>& M, std::size_t dim,
                        std::size_t maxRowsToShow = 5,
                        std::size_t maxColsToShow = 5,
                        int decimalPrecision = 3) {
    
    std::size_t rowsToShow = std::min(dim, maxRowsToShow);
    std::size_t colsToShow = std::min(dim, maxColsToShow);
    
    std::cout << "Matrix (showing " << rowsToShow << "×" << colsToShow 
              << " of " << dim << "×" << dim << "):\n";
    
    std::cout << std::fixed << std::setprecision(decimalPrecision);
    
    for (std::size_t i = 0; i < rowsToShow; ++i) {
        std::cout << "  [";
        for (std::size_t j = 0; j < colsToShow; ++j) {
            if (j > 0) std::cout << ", ";
            std::cout << std::setw(6 + decimalPrecision) << M[i][j];
        }
        if (dim > colsToShow) std::cout << ", ...";
        std::cout << "]\n";
    }
    if (dim > rowsToShow) {
        std::cout << "  ...\n";
    }
}

// -----------------------------------------------------------------------------
// Verify FHE matrix-vector multiplication result
// -----------------------------------------------------------------------------
inline void verify_matrix_vector_result(
    const std::vector<double>& fheResult,
    const Matrix<double>& M,
    const std::vector<double>& inputVec,
    std::size_t matrixDim,
    std::size_t numElementsToShow = 8,  // Default: show 8 elements
    int decimalPrecision = 2) {         // Default: 2 decimal places
    
    // Compute expected result (only first matrixDim elements)
    std::vector<double> inputOriginal(inputVec.begin(), inputVec.begin() + matrixDim);
    auto expected = matrix_vector_multiply(M, inputOriginal, matrixDim);
    
    // Determine how many elements to display
    std::size_t elementsToShow = std::min(matrixDim, numElementsToShow);
    
    // Compare results with nice formatting
    std::cout << "\nResults (first " << elementsToShow << " elements):\n";
    std::cout << std::fixed << std::setprecision(decimalPrecision);
    
    // Print FHE results
    std::cout << "FHE:      [";
    for (std::size_t i = 0; i < elementsToShow; ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << std::setw(7 + decimalPrecision) << fheResult[i];
    }
    if (matrixDim > elementsToShow) std::cout << ", ...";
    std::cout << "]\n";
    
    // Print expected results
    std::cout << "Expected: [";
    for (std::size_t i = 0; i < elementsToShow; ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << std::setw(7 + decimalPrecision) << expected[i];
    }
    if (matrixDim > elementsToShow) std::cout << ", ...";
    std::cout << "]\n";
    
    // Calculate error metrics
    double maxError = 0.0;
    double avgError = 0.0;
    std::size_t maxErrorIdx = 0;
    
    for (std::size_t i = 0; i < matrixDim; ++i) {
        double error = std::abs(fheResult[i] - expected[i]);
        if (error > maxError) {
            maxError = error;
            maxErrorIdx = i;
        }
        avgError += error;
    }
    avgError /= matrixDim;
    
    // Print error analysis
    std::cout << "\nError Analysis:\n";
    std::cout << "  Max error: " << std::scientific << std::setprecision(3) << maxError 
              << " (at index " << maxErrorIdx << ")\n";
    std::cout << "  Avg error: " << std::scientific << std::setprecision(3) << avgError << "\n";
    
    // Simple pass/fail indicator
    const double ERROR_THRESHOLD = 1e-3;
    if (maxError < ERROR_THRESHOLD) {
        std::cout << "  Status: ✓ PASS (errors within threshold of " 
                  << ERROR_THRESHOLD << ")\n";
    } else {
        std::cout << "  Status: ✗ FAIL (max error exceeds threshold of " 
                  << ERROR_THRESHOLD << ")\n";
    }
}

// -----------------------------------------------------------------------------
// Helper: rotate a vector down (to the right) by k positions, cyclically
// -----------------------------------------------------------------------------
inline std::vector<double> rotateVectorDown(const std::vector<double>& vec, int k) {
    const int n = static_cast<int>(vec.size());
    if (n == 0) return {};

    k %= n;                // handle k ≥ n
    if (k < 0) k += n;     // ensure k is non-negative

    std::vector<double> rotated(n);
    for (int i = 0; i < n; ++i) {
        // element originally at index i moves to (i + k) % n
        rotated[(i + k) % n] = vec[i];
    }
    return rotated;
}

#endif // OPENFHE_BENCHMARKS_UTILS_HPP