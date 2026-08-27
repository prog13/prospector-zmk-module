# Host tests

Tests for the parts of this module that are free of Zephyr, so they need nothing but a
host compiler. Each test is one `.c` file that returns a non-zero exit code when a case
fails.

Run them from the repository root:

```sh
./tests/run.sh
```

The script reads `PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL`'s default out of `Kconfig` and
compiles it in, so the tests run at the travel the firmware ships with. Changing the
default is expected to fail the cases that turn on the size of a level; re-derive them
rather than scaling the numbers.

- `touch_brightness_drag_test.c`: cases for the drag reducer, one behaviour each.
