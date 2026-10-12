Source files in this subdirectory contain unit tests.
They use gTest and gMock for running unit testing.
Unit tests can be launched with "dosbox-x -tests".
Free free to add more unit tests in this directory.

Read Google Test Primer for reference of most available
features, macros, and guidance about writing unit tests:

https://google.github.io/googletest/primer.html

More information about gMock can be found at:

https://google.github.io/googletest/gmock_for_dummies.html

Guest-control bounded frame receipt tests (no emulator/guest launch):
  python3 -m unittest discover -s tests -p guest_control_cpu_frames_test.py -v
These compile the actual parser, CPU capture block and receipt serializer with
small host/emulator dependency substitutes, then check legacy CPU STATE output,
optional CPU STATE 0 FRAMES, malformed syntax, failed memory reads and the output
queue bound. FRAMES captures EBP plus 1024 stack words in one guest-control event;
the original stack key retains its first128 words. The4KiB snapshot is a bounded
source-frame observation, not a universal backtrace. Missing/cyclic/out-of-range
frames cannot establish completion. No guest instruction executes between this
event's register capture and bounded memory reads.
