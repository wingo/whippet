#ifndef DEBUG_H
#define DEBUG_H

#ifndef NDEBUG
#define DEBUG(...) fprintf (stderr, "DEBUG: " __VA_ARGS__)
#else
#define DEBUG(...) do { } while (0)
#endif

#endif // DEBUG_H
