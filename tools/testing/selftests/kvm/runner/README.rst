KVM Selftest Runner
===================

KVM selftest runner is highly configurable test executor that allows to run
tests with different configurations (not just the default), parallely, save
output to disk hierarchically, control what gets printed on console, provide
execution status.

To generate default tests use::

  # make tests_install

This will create ``testcases_default_gen`` directory which will have testcases
in `default.test` files. Each KVM selftest will have a directory in  which
`default.test` file will be created with executable path relative to KVM
selftest root directory i.e. `/tools/testing/selftests/kvm`. For example, the
`dirty_log_perf_test` will have::

  # cat testcase_default_gen/dirty_log_perf_test/default.test
  dirty_log_perf_test

Runner will execute `dirty_log_perf_test`. Testcases files can also provide
extra arguments to the test::

  # cat tests/dirty_log_perf_test/2slot_5vcpu_10iter.test
  dirty_log_perf_test -x 2 -v 5 -i 10

In this case runner will execute the `dirty_log_perf_test` with the options.

Example
=======

To see all of the options::

  # python3 runner -h

To run all of the default tests::

  # python3 runner -d testcases_default_gen

To run tests parallely::

  # python3 runner -d testcases_default_gen -j 40

To print only passed test status and failed test stderr::

  # python3 runner -d testcases_default_gen --print-passed status \
  --print-failed stderr

To run tests binary which are in some other directory (out of tree builds)::

  # python3 runner -d testcases_default_gen -p /path/to/binaries


