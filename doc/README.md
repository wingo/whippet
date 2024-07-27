# Whippet documentation

 * [Manual](./manual.md): How do you get your program to use
   Whippet?  What is the API?

 * [Collector implementations](./collectors.md): There are a number of
   implementations of the Whippet API with differing performance
   characteristics and which impose different requirements on the
   embedder.
   - [Semi-space collector (semi)](./collector-semi.md): For
     single-threaded embedders who are not too tight on memory.
   - [Parallel copying collector (pcc)](./collector-pcc.md): Like semi,
     but with support for multiple mutator threads.  Faster than semi if
     multiple cores are available at collection-time.
   - [Whippet collector (whippet)](./collector-whippet.md):
     Immix-inspired collector.  Optionally parallel, conservative (stack
     and/or heap), and/or generational.
   - [Boehm-Demers-Weiser collector (bdw)](./collector-bdw.md):
     Conservative mark-sweep collector, implemented by
     Boehm-Demers-Weiser library.

 * [Guile](./doc/guile.md): Some notes on a potential rebase of Guile on
   top of Whippet.

