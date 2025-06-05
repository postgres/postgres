#!/bin/bash

# =====================================================
# TOAST Optimization Benchmark Runner
# =====================================================
# This script runs performance tests before and after
# enabling the toast optimization feature

set -e

# Configuration
DB_NAME=${DB_NAME:-postgres}
DB_USER=${DB_USER}
DB_HOST=${DB_HOST:-localhost}
DB_PORT=${DB_PORT:-5432}

# Test parameters
TEST_ROUNDS=${TEST_ROUNDS:-3}
OUTPUT_DIR="toast_benchmark_results"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
  echo -e "${BLUE}========================================${NC}"
  echo -e "${BLUE} $1${NC}"
  echo -e "${BLUE}========================================${NC}"
}

print_step() {
  echo -e "${GREEN}[STEP]${NC} $1"
}

print_warning() {
  echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
  echo -e "${RED}[ERROR]${NC} $1"
}

# Check if PostgreSQL is running and accessible
check_postgres() {
  print_step "Checking PostgreSQL connection..."
  if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -c "SELECT version();" >/dev/null 2>&1; then
    print_error "Cannot connect to PostgreSQL. Please check connection parameters."
    exit 1
  fi
  print_step "PostgreSQL connection successful"
}

# Create output directory
setup_output_dir() {
  mkdir -p "$OUTPUT_DIR"
  print_step "Created output directory: $OUTPUT_DIR"
}

# Run a single test
run_test() {
  local test_name="$1"
  local optimization_enabled="$2"
  local output_file="$3"

  print_step "Running test: $test_name (optimization: $optimization_enabled)"

  # Create a temporary SQL file with the test
  local temp_sql=$(mktemp)

  cat >"$temp_sql" <<EOF
-- Set optimization setting
SET enable_toast_batch_optimization = $optimization_enabled;

-- Show current settings
SELECT
    name,
    setting,
    context
FROM pg_settings
WHERE name IN ('enable_toast_batch_optimization', 'shared_buffers', 'effective_cache_size');

-- Include the main test script
\i toast_performance_test.sql
EOF

  # Run the test and capture output
  echo "Test: $test_name" >"$output_file"
  echo "Optimization enabled: $optimization_enabled" >>"$output_file"
  echo "Timestamp: $(date)" >>"$output_file"
  echo "===========================================" >>"$output_file"

  psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
    -f "$temp_sql" >>"$output_file" 2>&1

  # Clean up
  rm -f "$temp_sql"

  print_step "Test completed: $output_file"
}

# Extract timing results from output
extract_timings() {
  local file="$1"
  local test_type="$2"

  echo "=== $test_type Timings ==="
  grep -E "Time: [0-9]+\.[0-9]+ ms" "$file" | head -20
  echo
}

# Compare results
compare_results() {
  local before_file="$1"
  local after_file="$2"

  print_header "PERFORMANCE COMPARISON SUMMARY"

  echo "Before optimization (enable_toast_batch_optimization = off):"
  extract_timings "$before_file" "BEFORE"

  echo "After optimization (enable_toast_batch_optimization = on):"
  extract_timings "$after_file" "AFTER"

  # Extract IO statistics
  print_header "IO STATISTICS COMPARISON"

  echo "BEFORE - Toast IO Statistics:"
  grep -A 10 -B 2 "toast_blks_read" "$before_file" | head -15
  echo

  echo "AFTER - Toast IO Statistics:"
  grep -A 10 -B 2 "toast_blks_read" "$after_file" | head -15
  echo
}

# Generate summary report
generate_report() {
  local summary_file="$OUTPUT_DIR/benchmark_summary.txt"

  print_step "Generating summary report: $summary_file"

  cat >"$summary_file" <<EOF
TOAST Optimization Benchmark Summary
Generated: $(date)
Database: $DB_NAME@$DB_HOST:$DB_PORT
Test Rounds: $TEST_ROUNDS

=== Test Configuration ===
- Small data: 1KB (no toast)
- Medium data: 4KB (below optimization threshold)
- Large data: 16KB (above optimization threshold)
- Huge data: 64KB (well above optimization threshold)

=== Key Metrics to Compare ===
1. Insert timing for large/huge records
2. Select timing for full detoasting
3. Slice access (substring) timing
4. Toast block reads (toast_blks_read)
5. Toast block read time (toast_blk_read_time)

EOF

  # Add comparison if both files exist
  if [[ -f "$OUTPUT_DIR/test_before_optimization.out" && -f "$OUTPUT_DIR/test_after_optimization.out" ]]; then
    compare_results "$OUTPUT_DIR/test_before_optimization.out" "$OUTPUT_DIR/test_after_optimization.out" >>"$summary_file"
  fi

  print_step "Summary report generated"
}

# Main execution
main() {
  print_header "TOAST OPTIMIZATION BENCHMARK"

  # Check prerequisites
  check_postgres
  setup_output_dir

  # Check if toast_performance_test.sql exists
  if [[ ! -f "toast_performance_test.sql" ]]; then
    print_error "toast_performance_test.sql not found in current directory"
    exit 1
  fi

  print_step "Starting benchmark with $TEST_ROUNDS rounds..."

  # Run tests with optimization disabled
  print_header "TESTING WITH OPTIMIZATION DISABLED"
  run_test "Before Optimization" "off" "$OUTPUT_DIR/test_before_optimization.out"

  # Run tests with optimization enabled
  print_header "TESTING WITH OPTIMIZATION ENABLED"
  run_test "After Optimization" "on" "$OUTPUT_DIR/test_after_optimization.out"

  # Generate comparison report
  generate_report

  print_header "BENCHMARK COMPLETED"
  print_step "Results saved in: $OUTPUT_DIR/"
  print_step "Summary: $OUTPUT_DIR/benchmark_summary.txt"

  print_warning "Review the timing differences and IO statistics to evaluate performance impact"
  print_warning "Look especially for reduced toast_blks_read and improved timing on large data"
}

# Handle command line arguments
case "${1:-}" in
--help | -h)
  echo "Usage: $0 [options]"
  echo "Options:"
  echo "  --help, -h     Show this help"
  echo "  --clean        Clean previous test results"
  echo ""
  echo "Environment variables:"
  echo "  DB_NAME        Database name (default: postgres)"
  echo "  DB_USER        Database user (default: postgres)"
  echo "  DB_HOST        Database host (default: localhost)"
  echo "  DB_PORT        Database port (default: 5432)"
  echo "  TEST_ROUNDS    Number of test rounds (default: 3)"
  exit 0
  ;;
--clean)
  print_step "Cleaning previous test results..."
  rm -rf "$OUTPUT_DIR"
  print_step "Cleaned"
  exit 0
  ;;
esac

# Run main function
main "$@"
