STYLE
-----
Remove excess indentation.  Don't indent for namespace.

Add spaces around '=', ',', '<<', etc.

Remove internal jlib:: tokens in .cc files.  Everything should be under that namespace anyway.

Move all inline code to bottom of header.

CODE
----
Start moving away from libsigc++ in favor of std::function et alia.  jlib/apps/jlib-mail.cc is a good place to start.

Verify certs in SSL code.

