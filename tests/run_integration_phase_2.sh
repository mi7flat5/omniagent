#!/bin/bash
# Integration test runner for Phase 2: Dispatch Optimizer

set -e

echo "Running Phase 2 Dispatch Optimizer Integration Tests..."

# Change to project root directory
cd "$(dirname "$0")/.."

# Run pytest with verbose output
python -m pytest tests/integration_phase_2.py -v --tb=short

echo "Phase 2 integration tests completed."
