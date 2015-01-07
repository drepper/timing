# timing
Simple wrapper application to measure execution time of child process

Nothing fancy, it just measures time absolute time as provided by the
system (wall clock and CPU time).  The program is run multiple times
(configurable) and basic statistics are run.

If you look at the code you'll see that the arithmetic is performed
with integers.  I don't like floating point for tasks like this, we
can use arbitrary precision integers.

Using the program is easy.  It accepts options followed by the program
name and the parameters passed to the program.  No parameter reordering
takes place.  To be sure to interpret the parameters correctly terminate
the list of options to the `timing` with `--`.

Since I/O can influence the timing you might want to avoid writing
results to the terminal.  Use this by redirecting to `/dev/null`.  You
will still see the result of the program since it will write the
result to the terminal and not standard output.

Example:

    $ ./timing sort /usr/share/dict/words > /dev/null
    Strip out best and worst realtime result
    minimum: 0.075250396 sec real / 0.000079168 sec CPU
    maximum: 0.114116522 sec real / 0.000113514 sec CPU
    average: 0.094580758 sec real / 0.000091233 sec CPU
    stdev  : 0.007892835 sec real / 0.000005015 sec CPU

The output or `sort` is discarded but the timing information is visible.

