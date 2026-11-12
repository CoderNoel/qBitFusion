# qBitFusion: runs a QtTest program with its results written to a file, then prints that file.
# Some tests (those that start a libtorrent session) leave nothing on stdout under CTest, so without
# this a failure in CI shows no reason. Usage: cmake -DTEST_EXE=<path> -DLOG_FILE=<path> -P runwithlog.cmake
file(REMOVE "${LOG_FILE}")
execute_process(COMMAND "${TEST_EXE}" -o "${LOG_FILE},txt" RESULT_VARIABLE result)
if (EXISTS "${LOG_FILE}")
    file(READ "${LOG_FILE}" log)
    message("${log}")
endif()
if (NOT result EQUAL 0)
    message(FATAL_ERROR "${TEST_EXE} failed: ${result}")
endif()
