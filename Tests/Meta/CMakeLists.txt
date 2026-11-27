ladybird_python_test(test-release-configuration.py ENVIRONMENT "LADYFLOCK_TEST_CMAKE=${CMAKE_COMMAND}")

if (NOT WIN32)
    ladybird_python_test(test-ipc-generator.py)
    ladybird_python_test(test-js-runner.py)
    ladybird_python_test(test-lint-environment.py)
    ladybird_python_test(test-xctrace-time-profile.py)
endif()

if (APPLE)
    ladybird_python_test(test-package-macos-test-bundle.py)
endif()
