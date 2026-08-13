STYLE
-----
Remove excess indentation.  Don't indent for namespace.

Add spaces around '=', ',', '<<', etc.

Remove internal jlib:: tokens in .cc files.  Everything should be under that namespace anyway.

Move all inline code to bottom of header.

CODE
----
Both former entries here are done: libsigc++ is gone in favour of std::function
and jlib/sys/signal.hh, and the SSL code verifies the peer certificate against
the system trust store and checks the hostname with SSL_set1_host.

Remaining work is tracked in GitHub issues rather than here.

