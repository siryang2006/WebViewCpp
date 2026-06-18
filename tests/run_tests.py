"""C++ service test runner for WebViewCpp.
Runs built-in C++ unit tests via --test flag.
"""
import subprocess
import sys
import os

EXE_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                        "build", "Debug", "WebViewCpp.exe")


def run_tests():
    if not os.path.exists(EXE_PATH):
        print(f"ERROR: {EXE_PATH} not found")
        return False

    print(f"Running: {EXE_PATH} --test")
    try:
        result = subprocess.run([EXE_PATH, "--test"],
                                capture_output=True,
                                text=True,
                                timeout=60)
        output = result.stdout
    except subprocess.TimeoutExpired:
        print("ERROR: Test timed out")
        return False
    except Exception as e:
        print(f"ERROR: {e}")
        return False

    print(output)

    # Check results
    if "FAIL" in output and "Failed: 0" not in output:
        print("WARNING: Some tests FAILED")
        return False

    # Verify expected test results (all 35 tests should be present)
    expected = [
        "math.add(10,20)",
        "math.multiply(6,7)",
        "math.version",
        "math.pi",
        "math.error_method_not_found",
        "math.error_property_not_found",
        "math.error_set_readonly",
        "math.add_wrong_arg_type",
        "file.object_name",
        "file.read_is_async",
        "file.write_is_async",
        "Worker.getName",
        "Worker.getPriority",
        "Worker.setPriority(8)",
        "Worker.getPriority_after_set",
        "Worker.error_method_not_found",
        "Worker.error_property_not_found",
        "Worker.error_set_non_existent",
        "Worker.default_name",
        "Worker.default_priority",
        "Worker.partial_arg_name",
        "Worker.partial_arg_priority",
        "Worker.multi_A_priority",
        "Worker.multi_B_priority",
        "Worker.unique_instance_ids",
        "Worker.doWork_is_async",
        "CppObject.ok_result",
        "CppObject.error_result",
        "download.object_name",
        "download.startDownload_is_async",
        "download.pauseDownload_no_task",
        "download.resumeDownload_no_task",
        "download.cancelDownload_no_task",
        "download.getProgress_no_task",
        "download.getSpeed_no_task",
        "download.pause_empty_modelId",
        "download.full_download",
        "download.resume_206_append",
        "download.no_range_truncates",
        "download.pause_resume",
        "download.cancel_removes_task",
    ]

    for name in expected:
        if f"[PASS] {name}" not in output:
            print(f"WARNING: Missing test: {name}")
            return False

    print("All tests PASSED")
    return True


if __name__ == "__main__":
    ok = run_tests()
    sys.exit(0 if ok else 1)
