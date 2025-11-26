#!/usr/bin/env python3
"""
RayForce Documentation Test Script

Tests all code examples in markdown documentation using the rayforce REPL.

Usage:
    python docs/tests/test_docs.py                    # Test all docs
    python docs/tests/test_docs.py docs/docs/content/math/add.md  # Test single file
    python docs/tests/test_docs.py --report           # Generate detailed report
    python docs/tests/test_docs.py --extract-only     # Only extract examples, no testing
"""

import os
import sys
import re
import json
import argparse
import subprocess
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Any
from enum import Enum


SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent
RAYFORCE_PATH = PROJECT_ROOT.parent  # Root where rayforce binary is
EXPECT_SCRIPT = SCRIPT_DIR / "run_batch.expect"


class TestStatus(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    ERROR = "ERROR"
    SKIP = "SKIP"
    EXTRACT_ONLY = "EXTRACT"


@dataclass
class CodeExample:
    """A code example extracted from documentation"""
    file_path: str
    line_number: int
    expression: str
    expected_output: Optional[str] = None
    is_setup: bool = False  # True if this is a (set ...) expression


@dataclass
class TestResult:
    """Result of testing a code example"""
    example: CodeExample
    status: TestStatus
    actual_output: Optional[str] = None
    error_message: Optional[str] = None


@dataclass
class FileReport:
    """Test report for a single file"""
    file_path: str
    results: List[TestResult] = field(default_factory=list)
    examples_extracted: int = 0

    @property
    def total(self) -> int:
        return len(self.results)

    @property
    def passed(self) -> int:
        return sum(1 for r in self.results if r.status == TestStatus.PASS)

    @property
    def failed(self) -> int:
        return sum(1 for r in self.results if r.status == TestStatus.FAIL)

    @property
    def errors(self) -> int:
        return sum(1 for r in self.results if r.status == TestStatus.ERROR)


def run_expressions_batch(expressions: List[str]) -> List[Tuple[Optional[str], Optional[str]]]:
    """
    Run a batch of expressions using the expect script.

    Returns list of (result, error) tuples.
    """
    if not expressions:
        return []

    # Check expect script exists
    if not EXPECT_SCRIPT.exists():
        raise FileNotFoundError(f"Expect script not found: {EXPECT_SCRIPT}")

    # Prepare input
    input_text = '\n'.join(expressions)

    # Run expect script
    try:
        result = subprocess.run(
            [str(EXPECT_SCRIPT)],
            input=input_text,
            capture_output=True,
            text=True,
            timeout=60,
            cwd=str(RAYFORCE_PATH)
        )

        # Parse JSON output
        try:
            output = json.loads(result.stdout)
        except json.JSONDecodeError as e:
            # Return error for all expressions
            return [(None, f"JSON parse error: {e}\nOutput: {result.stdout[:500]}")] * len(expressions)

        # Build results list using positional matching
        # (duplicate expressions get different results based on position)
        results = []

        for i, expr in enumerate(expressions):
            if i < len(output):
                item = output[i]
                # Verify the expression matches (sanity check)
                if item.get('expr') != expr:
                    results.append((None, f"Expression mismatch at position {i}"))
                    continue
                if 'error' in item:
                    results.append((None, item['error']))
                elif item.get('result') is None:
                    results.append((None, None))  # No output
                else:
                    result_str = item['result']
                    # Check if it's an error
                    if result_str.startswith('••'):
                        results.append((None, result_str))
                    else:
                        results.append((result_str, None))
            else:
                results.append((None, "Expression not found in output"))

        return results

    except subprocess.TimeoutExpired:
        return [(None, "Timeout executing expressions")] * len(expressions)
    except Exception as e:
        return [(None, str(e))] * len(expressions)


def extract_error_code(text: str) -> Optional[str]:
    """Extract error code like [E017] from error output."""
    if not text:
        return None
    match = re.search(r'\[E\d+\]', text)
    return match.group(0) if match else None


def extract_code_blocks(file_path: str) -> List[CodeExample]:
    """
    Extract code examples from a markdown file.

    Looks for code blocks marked with ```clj or ``` and parses
    input/output pairs where lines starting with ↪ are inputs.
    """
    examples = []

    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()
        lines = content.split('\n')

    # Find all code blocks
    in_code_block = False
    code_block_start = 0
    code_block_lang = ""
    code_lines = []

    for i, line in enumerate(lines):
        # Check for code block start
        if line.strip().startswith('```'):
            if not in_code_block:
                # Starting a code block
                in_code_block = True
                code_block_start = i + 1  # 1-indexed line number
                code_block_lang = line.strip()[3:].strip()
                code_lines = []
            else:
                # Ending a code block
                in_code_block = False

                # Only process clj code blocks or unmarked ones
                if code_block_lang in ('clj', 'lisp', 'clojure', ''):
                    # Parse the code block for input/output pairs
                    block_examples = parse_code_block(
                        file_path,
                        code_block_start,
                        code_lines
                    )
                    examples.extend(block_examples)
        elif in_code_block:
            code_lines.append(line)

    return examples


def count_parens(s: str) -> int:
    """Count net parentheses (open - close), ignoring those in strings.

    Note: In RayForce, ' is a quote operator for symbols, not a string delimiter.
    Only " is a string delimiter.
    """
    in_string = False
    count = 0
    i = 0
    while i < len(s):
        c = s[i]
        if not in_string:
            if c == '"':
                in_string = True
            elif c == '(':
                count += 1
            elif c == ')':
                count -= 1
        else:
            if c == '"' and (i == 0 or s[i-1] != '\\'):
                in_string = False
        i += 1
    return count


def parse_code_block(file_path: str, start_line: int, lines: List[str]) -> List[CodeExample]:
    """
    Parse a code block to extract input/output pairs.

    Lines starting with ↪ are treated as inputs.
    Following lines (until next ↪ or end) are expected output.
    Lines starting with ;; are comments.
    Multiline expressions are supported - continues until parens balance.
    """
    examples = []
    current_expr = None
    current_line = start_line
    expected_lines = []
    expr_complete = True  # Track if expression parentheses are balanced

    for i, line in enumerate(lines):
        stripped = line.strip()

        # Skip empty lines
        if not stripped:
            continue

        # Skip pure comment lines
        if stripped.startswith(';;'):
            continue

        # Check if this is a new input line (with ↪ prefix)
        if stripped.startswith('↪'):
            # Save previous example if exists
            if current_expr is not None:
                is_setup = current_expr.strip().startswith('(set ')
                examples.append(CodeExample(
                    file_path=file_path,
                    line_number=current_line,
                    expression=current_expr,
                    expected_output='\n'.join(expected_lines).strip() if expected_lines else None,
                    is_setup=is_setup
                ))

            # Start new example
            current_expr = stripped[1:].strip()  # Remove ↪ prefix
            current_line = start_line + i
            expected_lines = []
            expr_complete = count_parens(current_expr) == 0

        elif current_expr is not None:
            if not expr_complete:
                # Continue accumulating the multiline expression
                current_expr += ' ' + stripped
                expr_complete = count_parens(current_expr) == 0
            else:
                # This is expected output for current expression
                expected_lines.append(line)

    # Save last example
    if current_expr is not None:
        is_setup = current_expr.strip().startswith('(set ')
        examples.append(CodeExample(
            file_path=file_path,
            line_number=current_line,
            expression=current_expr,
            expected_output='\n'.join(expected_lines).strip() if expected_lines else None,
            is_setup=is_setup
        ))

    return examples


def test_examples(examples: List[CodeExample]) -> List[TestResult]:
    """Test a list of code examples."""
    if not examples:
        return []

    # Extract expressions
    expressions = [ex.expression for ex in examples]

    # Run all expressions in batch
    results = run_expressions_batch(expressions)

    # Build test results
    test_results = []
    for example, (result_str, error) in zip(examples, results):
        test_result = evaluate_result(example, result_str, error)
        test_results.append(test_result)

    return test_results


def evaluate_result(example: CodeExample, result_str: Optional[str], error: Optional[str]) -> TestResult:
    """Evaluate a single test result."""
    # Check if expected output indicates an error (starts with ••)
    expects_error = (example.expected_output and
                     example.expected_output.strip().startswith('••'))

    if error:
        if expects_error:
            # Compare error codes
            expected_code = extract_error_code(example.expected_output)
            actual_code = extract_error_code(error)

            if expected_code and actual_code and expected_code == actual_code:
                return TestResult(
                    example=example,
                    status=TestStatus.PASS,
                    actual_output=error
                )
            else:
                return TestResult(
                    example=example,
                    status=TestStatus.FAIL,
                    actual_output=error,
                    error_message=f"Error code mismatch: expected {expected_code}, got {actual_code}"
                )
        else:
            # Unexpected error
            return TestResult(
                example=example,
                status=TestStatus.ERROR,
                error_message=error
            )

    # If we expected an error but didn't get one
    if expects_error:
        return TestResult(
            example=example,
            status=TestStatus.FAIL,
            actual_output=result_str,
            error_message="Expected error but got success"
        )

    # If no expected output, just check that it executed without error
    if not example.expected_output:
        return TestResult(
            example=example,
            status=TestStatus.PASS,
            actual_output=result_str
        )

    # Compare output (normalize whitespace)
    expected_normalized = normalize_output(example.expected_output)
    actual_normalized = normalize_output(result_str) if result_str else ""

    if expected_normalized == actual_normalized:
        return TestResult(
            example=example,
            status=TestStatus.PASS,
            actual_output=result_str
        )
    else:
        return TestResult(
            example=example,
            status=TestStatus.FAIL,
            actual_output=result_str,
            error_message=f"Output mismatch"
        )


def normalize_float(s: str) -> str:
    """Normalize float representation by removing trailing zeros after decimal."""
    import re
    def norm_float(match):
        num = match.group(0)
        if '.' in num:
            parts = num.split('.')
            integer_part = parts[0]
            decimal_part = parts[1].rstrip('0') or '0'
            return f"{integer_part}.{decimal_part}"
        return num
    return re.sub(r'-?\d+\.\d+', norm_float, s)


def normalize_output(output: str) -> str:
    """Normalize output for comparison."""
    if not output:
        return ""
    # Remove leading/trailing whitespace from each line
    lines = [line.strip() for line in output.strip().split('\n')]
    # Remove empty lines
    lines = [line for line in lines if line]
    result = '\n'.join(lines)
    # Normalize float representations
    result = normalize_float(result)
    return result


def test_file(file_path: str, extract_only: bool = False) -> FileReport:
    """Test all examples in a file."""
    report = FileReport(file_path=file_path)

    examples = extract_code_blocks(file_path)
    report.examples_extracted = len(examples)

    if extract_only:
        for example in examples:
            report.results.append(TestResult(
                example=example,
                status=TestStatus.EXTRACT_ONLY
            ))
        return report

    # Test all examples in this file as a batch
    results = test_examples(examples)
    report.results = results

    return report


def find_doc_files(base_path: str) -> List[str]:
    """Find all markdown files in the docs directory."""
    doc_files = []
    base = Path(base_path)

    for md_file in base.rglob('*.md'):
        doc_files.append(str(md_file))

    return sorted(doc_files)


def print_report(reports: List[FileReport], verbose: bool = False, extract_only: bool = False):
    """Print test report summary."""
    total_files = len(reports)
    total_examples = sum(r.examples_extracted for r in reports)
    total_tests = sum(r.total for r in reports)
    total_passed = sum(r.passed for r in reports)
    total_failed = sum(r.failed for r in reports)
    total_errors = sum(r.errors for r in reports)

    print("\n" + "=" * 60)
    print("RayForce Documentation Test Report")
    print("=" * 60)
    print(f"\nFiles scanned: {total_files}")
    print(f"Total examples extracted: {total_examples}")

    if not extract_only:
        print(f"Tests executed: {total_tests}")
        print(f"  Passed: {total_passed}")
        print(f"  Failed: {total_failed}")
        print(f"  Errors: {total_errors}")

    # List files with failures
    if not extract_only:
        failed_files = [r for r in reports if r.failed > 0 or r.errors > 0]

        if failed_files:
            print("\n" + "-" * 60)
            print("Files with failures:")
            print("-" * 60)

            for report in failed_files:
                rel_path = os.path.relpath(report.file_path, PROJECT_ROOT)
                print(f"\n{rel_path}: {report.failed} failed, {report.errors} errors")

                if verbose:
                    for result in report.results:
                        if result.status in (TestStatus.FAIL, TestStatus.ERROR):
                            print(f"  Line {result.example.line_number}:")
                            expr_preview = result.example.expression[:60]
                            if len(result.example.expression) > 60:
                                expr_preview += "..."
                            print(f"    Expression: {expr_preview}")
                            if result.error_message:
                                print(f"    Error: {result.error_message}")
                            if result.example.expected_output and result.actual_output:
                                exp_preview = result.example.expected_output[:40]
                                act_preview = result.actual_output[:40] if result.actual_output else ""
                                print(f"    Expected: {exp_preview}...")
                                print(f"    Actual: {act_preview}...")

    print("\n" + "=" * 60)

    # Return exit code
    if extract_only:
        return 0
    return 0 if total_failed == 0 and total_errors == 0 else 1


def save_json_report(reports: List[FileReport], output_path: str):
    """Save detailed report as JSON."""
    data = {
        'summary': {
            'total_files': len(reports),
            'total_examples': sum(r.examples_extracted for r in reports),
            'total_tests': sum(r.total for r in reports),
            'passed': sum(r.passed for r in reports),
            'failed': sum(r.failed for r in reports),
            'errors': sum(r.errors for r in reports),
        },
        'files': []
    }

    for report in reports:
        file_data = {
            'path': os.path.relpath(report.file_path, PROJECT_ROOT),
            'examples_extracted': report.examples_extracted,
            'total': report.total,
            'passed': report.passed,
            'failed': report.failed,
            'errors': report.errors,
            'results': []
        }

        for result in report.results:
            file_data['results'].append({
                'line': result.example.line_number,
                'expression': result.example.expression,
                'expected': result.example.expected_output,
                'actual': result.actual_output,
                'status': result.status.value,
                'error': result.error_message,
                'is_setup': result.example.is_setup
            })

        data['files'].append(file_data)

    with open(output_path, 'w') as f:
        json.dump(data, f, indent=2)

    print(f"\nDetailed report saved to: {output_path}")


def main():
    parser = argparse.ArgumentParser(description='Test RayForce documentation examples')
    parser.add_argument('files', nargs='*', help='Specific files to test (default: all docs)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show detailed failure info')
    parser.add_argument('--report', '-r', action='store_true', help='Save JSON report')
    parser.add_argument('--output', '-o', default='doc_test_report.json', help='JSON report output path')
    parser.add_argument('--extract-only', '-e', action='store_true', help='Only extract examples, no testing')

    args = parser.parse_args()

    # Check expect script exists
    if not args.extract_only and not EXPECT_SCRIPT.exists():
        print(f"Error: Expect script not found: {EXPECT_SCRIPT}")
        sys.exit(1)

    # Determine files to test
    if args.files:
        doc_files = args.files
    else:
        docs_path = PROJECT_ROOT / "docs" / "content"
        doc_files = find_doc_files(str(docs_path))

    print(f"Found {len(doc_files)} documentation files")

    # Run tests
    reports = []
    for file_path in doc_files:
        if not os.path.exists(file_path):
            print(f"Warning: File not found: {file_path}")
            continue

        report = test_file(file_path, extract_only=args.extract_only)
        reports.append(report)

        # Print progress
        rel_path = os.path.relpath(file_path, PROJECT_ROOT)
        if args.extract_only:
            print(f"  [EXTRACT] {rel_path}: {report.examples_extracted} examples")
        else:
            status = "PASS" if report.failed == 0 and report.errors == 0 else "FAIL"
            print(f"  [{status}] {rel_path}: {report.passed}/{report.total} passed")

    # Print summary
    exit_code = print_report(reports, verbose=args.verbose, extract_only=args.extract_only)

    # Save JSON report if requested
    if args.report:
        save_json_report(reports, args.output)

    sys.exit(exit_code)


if __name__ == '__main__':
    main()
