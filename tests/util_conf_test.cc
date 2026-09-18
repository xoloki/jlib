/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What this pins, and what it deliberately does not.
 *
 * The parser knows no directives.  There is no "listen takes one argument"
 * here, because there is no such rule in the code -- a config file that says
 * `listen a b c d;` parses, and it is jhttpd's job to complain.  Sections that
 * look like they are about jhttpd are about *shape*: that a block nests, that
 * a quoted argument keeps its spaces, that a "#" inside quotes is not a
 * comment.
 *
 * Several sections assert that something does NOT parse.  Each one says why it
 * is worth a test, because a grammar that is merely *stricter* than intended
 * passes every positive test and fails silently on a real config two years
 * later.
 */

#include <jlib/util/conf.hh>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Does this text parse at all? */
static bool parses(const std::string& s) {
    try { conf::parse(s); return true; } catch(const conf::error&) { return false; }
}

/** The line a failure names, or 0 if it parsed. */
static std::size_t fails_at(const std::string& s) {
    try { conf::parse(s); return 0; } catch(const conf::error& e) { return e.line(); }
}

static void a_directive_is_a_name_some_arguments_and_a_semicolon() {
    std::cout << "directives:\n";

    std::vector<conf::directive> d = conf::parse("listen 8080;\n");

    ok("one directive", d.size() == 1, std::to_string(d.size()));
    ok("its name", d.size() == 1 && d[0].name == "listen");
    ok("its argument", d.size() == 1 && d[0].arg(0) == "8080");
    ok("and no block", d.size() == 1 && !d[0].blocked);

    d = conf::parse("daemon;");
    ok("a directive with no arguments at all", d.size() == 1 && d[0].args.empty());

    d = conf::parse("  \t listen\t8080 ;\n\n");
    ok("whitespace between the parts is free",
       d.size() == 1 && d[0].name == "listen" && d[0].arg(0) == "8080");

    d = conf::parse("a 1;\nb 2;\nc 3;\n");
    ok("several in a row", d.size() == 3 && d[2].name == "c");

    d = conf::parse("root /srv/www;");
    ok("a path is one argument", d.size() == 1 && d[0].arg(0) == "/srv/www");

    // The point of not listing allowed characters in `barechar`.  Each of
    // these is a real nginx argument and each would be rejected by a grammar
    // that spelled out what a token may contain.
    ok("and so is a version-numbered one",  parses("root /srv/www-2.0;"));
    ok("a regular expression",              parses("location ~ ^/api/[0-9]+$ {}"));
    ok("a MIME type",                       parses("default_type text/html;"));
    ok("an address with a colon",           parses("listen 127.0.0.1:8080;"));
    ok("a wildcard host",                   parses("server_name *.example.com;"));
    ok("a percent, as in a log format",     parses("log_format main %h;"));

    // A POSIX path is a byte string, so a directory nobody named in ASCII is
    // still a directory.  Nothing here decodes these -- they go in as bytes
    // and come out as the same bytes.
    {
        const std::string path = "/srv/\xc3\xa9t\xc3\xa9";
        std::vector<conf::directive> u = conf::parse("root " + path + ";");

        ok("a non-ASCII path is one argument, unchanged",
           u.size() == 1 && u[0].arg(0) == path, u.size() ? u[0].arg(0) : "");

        ok("a non-ASCII comment does not break the file",
           parses("# caf\xc3\xa9\nlisten 80;\n"));

        u = conf::parse("msg \"caf\xc3\xa9 au lait\";");
        ok("and non-ASCII inside quotes survives",
           u.size() == 1 && u[0].arg(0) == "caf\xc3\xa9 au lait");
    }

    ok("arg() past the end is empty, not a throw",
       conf::parse("daemon;")[0].arg(7) == "");
}

static void a_block_nests() {
    std::cout << "blocks:\n";

    std::vector<conf::directive> d = conf::parse("server {\n  listen 8080;\n}\n");

    ok("the block is one directive", d.size() == 1 && d[0].name == "server");
    ok("marked as blocked",          d.size() == 1 && d[0].blocked);
    ok("holding its contents",       d.size() == 1 && d[0].block.size() == 1);
    ok("which parsed normally",
       d.size() == 1 && d[0].block.size() == 1 && d[0].block[0].name == "listen");

    d = conf::parse("http { server { location / { root /srv; } } }");
    ok("three deep",
       d.size() == 1 && d[0].block.size() == 1 &&
       d[0].block[0].block.size() == 1 &&
       d[0].block[0].block[0].block.size() == 1 &&
       d[0].block[0].block[0].block[0].arg(0) == "/srv");

    d = conf::parse("server { listen 80; }\nserver { listen 443; }\n");
    ok("two blocks with the same name are two directives, not a merge",
       d.size() == 2 && d[0].block[0].arg(0) == "80"
                     && d[1].block[0].arg(0) == "443");

    d = conf::parse("location /x { }");
    ok("an empty block is still a block",
       d.size() == 1 && d[0].blocked && d[0].block.empty());

    d = conf::parse("location /static { root /srv; }");
    ok("a block keeps the arguments before it",
       d.size() == 1 && d[0].arg(0) == "/static");

    // A block is an *alternative* to the semicolon, so this is the one place
    // where a stray ";" is not harmless punctuation.
    ok("a block does not also take a semicolon", !parses("server { } ;"));
}

static void comments_and_quotes() {
    std::cout << "comments and quoting:\n";

    std::vector<conf::directive> d =
        conf::parse("# a leading comment\nlisten 8080;  # and a trailing one\n");

    ok("comments are not directives", d.size() == 1 && d[0].name == "listen");

    ok("a comment need not end in a newline", parses("listen 80;\n# at eof"));
    ok("a file that is only a comment",       parses("# nothing here\n"));
    ok("an empty file",                       parses(""));
    ok("a file of pure whitespace",           parses("\n\n  \t\n"));

    d = conf::parse("root \"/srv/my www\";");
    ok("a quoted argument keeps its space",
       d.size() == 1 && d[0].arg(0) == "/srv/my www");
    ok("and loses its quotes",
       d.size() == 1 && d[0].arg(0).find('"') == std::string::npos);

    d = conf::parse("log_format \"a # b\";");
    ok("a # inside quotes is not a comment",
       d.size() == 1 && d[0].arg(0) == "a # b");

    d = conf::parse("msg \"say \\\"hi\\\"\";");
    ok("an escaped quote",   d.size() == 1 && d[0].arg(0) == "say \"hi\"");

    d = conf::parse("msg \"a\\tb\\nc\";");
    ok("\\t and \\n become the characters",
       d.size() == 1 && d[0].arg(0) == "a\tb\nc");

    d = conf::parse("msg \"c:\\\\path\";");
    ok("an escaped backslash is one backslash",
       d.size() == 1 && d[0].arg(0) == "c:\\path");

    d = conf::parse("msg \"\";");
    ok("an empty quoted argument is an argument",
       d.size() == 1 && d[0].args.size() == 1 && d[0].arg(0).empty());

    // Tried quoted-first for exactly this reason: `bare` cannot contain a
    // quote, so a grammar that tried it first would match nothing here and
    // then fail the directive rather than backing into the quoted rule.
    d = conf::parse("msg \"a b\" c \"d e\";");
    ok("quoted and bare arguments mixed",
       d.size() == 1 && d[0].args.size() == 3 &&
       d[0].arg(0) == "a b" && d[0].arg(1) == "c" && d[0].arg(2) == "d e");

    ok("an unterminated quote does not parse", !parses("msg \"unclosed;\n"));
}

static void what_does_not_parse_and_where_it_says_so() {
    std::cout << "refusals:\n";

    // Each of these is a mistake somebody will actually make in a config file,
    // and the line number is the whole value of reporting it -- a parser that
    // says "syntax error" and nothing else is not worth writing.
    ok("a stray closing brace",   !parses("server { }\n}\n"));
    ok("an unclosed block",       !parses("server {\n  listen 80;\n"));
    ok("a bare name with no terminator at all", !parses("listen"));
    ok("a block with nothing before it",        !parses("{ listen 80; }"));

    // **A missing semicolon is not a syntax error, and must not be.**
    //
    // A newline is whitespace, so this is one `listen` with three arguments,
    // not two directives.  That is nginx's behaviour exactly -- its tokenizer
    // reads to the next ";" or "{" without caring about lines, and what
    // rejects the text above is the *arity check* on `listen`, one layer up.
    //
    // It has to work this way, because the continuation idiom below is real
    // nginx and common.  So catching a forgotten ";" is jhttpd's job when it
    // finds `listen` holding three arguments -- the parser cannot see it.
    {
        std::vector<conf::directive> d = conf::parse("listen 8080\nroot /srv;\n");

        ok("a missing semicolon parses as one directive, as in nginx",
           d.size() == 1 && d[0].name == "listen" && d[0].args.size() == 3,
           std::to_string(d.size()) + " directive(s)");

        d = conf::parse("log_format  main\n"
                        "            \"$remote_addr - $remote_user\"\n"
                        "            \"$request $status\";\n");

        ok("which is what lets a directive span lines",
           d.size() == 1 && d[0].args.size() == 3 &&
           d[0].arg(2) == "$request $status");
    }

    // Break-the-guard: if the line were hard-coded, or always the last line,
    // these two would agree.  They must not.
    ok("and two different mistakes do not report the same line",
       fails_at("}\n") != fails_at("a;\nb;\n}\n"));

    try {
        conf::parse("server { }\n}\n");
        ok("a refusal carries a message", false);
    } catch(const conf::error& e) {
        const std::string m = e.what();

        ok("a refusal names the line in what()",
           m.find("line 2") != std::string::npos, m);
        ok("and carries a column",  e.column() > 0, std::to_string(e.column()));
    }
}

/** The message a refusal carries, or "" if it parsed. */
static std::string says(const std::string& s) {
    try { conf::parse(s); return ""; } catch(const conf::error& e) { return e.what(); }
}

static void a_refusal_says_something_useful() {
    std::cout << "messages:\n";

    // These are the contract an operator actually meets, so they are pinned
    // rather than left to drift.  Two of them come from heuristics that the
    // expected list cannot produce -- a brace is wrong *because* of what is
    // not open, so the grammar never offers it as a possibility -- and a
    // heuristic that quietly stops firing is invisible without a test.
    struct { const char* what; const char* text; const char* wanted; } cases[] = {
        { "a missing terminator names both that could end it",
          "listen",                        "expected \";\" or \"{\"" },
        { "an unclosed block asks for the brace",
          "server {\n  listen 80;\n",      "expected \"}\"" },
        { "a stray brace is named as one",
          "server { }\n}\n",               "unmatched \"}\"" },
        { "a block with no name says so",
          "{ listen 80; }",                "a block needs a name before \"{\"" },
        { "an unterminated quote is diagnosed from the rule stack",
          "msg \"unclosed;\n",             "unterminated quoted argument" },
    };

    for(const auto& c : cases) {
        const std::string m = says(c.text);

        ok(c.what, m.find(c.wanted) != std::string::npos, m);
    }

    // Break-the-guard: the quote case is answered before the expected list is
    // consulted, so a valid quoted argument must not trip it.
    ok("and a good quoted argument does not",
       says("msg \"fine\";").empty(), says("msg \"fine\";"));
}

static void reading_a_file() {
    std::cout << "read():\n";

    const std::string path = "conf_test_tmp.conf";

    {
        std::ofstream out(path.c_str());
        out << "# written by the test\nserver {\n  listen 8080;\n}\n";
    }

    try {
        std::vector<conf::directive> d = conf::read(path);

        ok("a file off disk parses the same as a string",
           d.size() == 1 && d[0].block.size() == 1 && d[0].block[0].arg(0) == "8080");
    } catch(const conf::error& e) {
        ok("a file off disk parses the same as a string", false, e.what());
    }

    std::remove(path.c_str());

    // A missing config file is the single most likely failure in the field,
    // and it must not look like a syntax error: no line number, and the path
    // in the message, because "cannot read it" helps nobody.
    try {
        conf::read("no-such-file-here.conf");
        ok("a missing file is an error", false);
    } catch(const conf::error& e) {
        const std::string m = e.what();

        ok("a missing file is an error",      true);
        ok("naming the path",
           m.find("no-such-file-here.conf") != std::string::npos, m);
        ok("with no line number, since there is no line", e.line() == 0);
    }
}

static void a_config_that_looks_like_the_real_thing() {
    std::cout << "a whole file:\n";

    // Deliberately in nginx's own idiom rather than a shape invented here.
    // The stretch goal on #239 is reading a real nginx config, and this is the
    // cheapest possible check that the grammar is pointed at that.
    const std::string text =
        "# jhttpd\n"
        "user www-data www-data;\n"
        "daemon;\n"
        "\n"
        "http {\n"
        "    default_type  application/octet-stream;\n"
        "    log_format    main  \"$remote_addr - $remote_user\";\n"
        "\n"
        "    server {\n"
        "        listen       8080;\n"
        "        server_name  example.com *.example.com;\n"
        "        root         /srv/www;\n"
        "\n"
        "        location /static {   # served straight off disk\n"
        "            auth_basic  \"restricted area\";\n"
        "        }\n"
        "    }\n"
        "\n"
        "    server {\n"
        "        listen       8443 ssl;\n"
        "        server_name  other.example.com;\n"
        "    }\n"
        "}\n";

    std::vector<conf::directive> d;

    try {
        d = conf::parse(text);
    } catch(const conf::error& e) {
        ok("it parses", false, e.what());
        return;
    }

    ok("three top-level directives", d.size() == 3, std::to_string(d.size()));
    ok("the first is user",          d.size() == 3 && d[0].name == "user");
    ok("daemon has no arguments",    d.size() == 3 && d[1].args.empty());

    const std::vector<conf::directive>& http = d[2].block;

    ok("http holds four",            http.size() == 4, std::to_string(http.size()));
    ok("two of them are servers",
       http.size() == 4 && http[2].name == "server" && http[3].name == "server");
    ok("server_name keeps both names",
       http.size() == 4 && http[2].block[1].args.size() == 2);
    ok("the nested location survived",
       http.size() == 4 && http[2].block.size() == 4 &&
       http[2].block[3].name == "location" &&
       http[2].block[3].block[0].name == "auth_basic");
    ok("with its quoted argument intact",
       http.size() == 4 && http[2].block.size() == 4 &&
       http[2].block[3].block[0].arg(0) == "restricted area");
}

int main() {
    std::cout << std::unitbuf;

    try {
        a_directive_is_a_name_some_arguments_and_a_semicolon();
        a_block_nests();
        comments_and_quotes();
        what_does_not_parse_and_where_it_says_so();
        a_refusal_says_something_useful();
        reading_a_file();
        a_config_that_looks_like_the_real_thing();
    } catch(const std::exception& e) {
        std::cout << "  FAIL unexpected throw: " << e.what() << "\n";
        ++failures;
    }

    std::cout << (failures ? "util_conf_test: FAILED\n" : "util_conf_test: all good\n");

    // Not pinned here, and worth saying so:
    //
    // Not that any directive means anything.  Nothing in this file asserts
    // that `listen` takes a port, because the parser does not know that and
    // must not -- interpretation belongs to whoever reads the directives.
    //
    // Not nginx compatibility.  One realistic file parses; that is evidence,
    // not a guarantee.  nginx has no specification to test against, and the
    // features this grammar has never seen -- variables, "include", maps,
    // if blocks -- are unexamined rather than working.
    return failures ? 1 : 0;
}
