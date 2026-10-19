#pragma once
#include <string>

// It basically runs the Risk front-end (lexer, parser, (optional) typechecker)
// over this `src` and returns a JSON document describing the result:
//
//   { "diagnostics": [ {s,m,l,c,e} ... ],      // s: 1=error 2=warning, 1-based
//   l/c, e = inclusive end col
//     "symbols":     [ {k,n,d,l,c,...} ... ],  // every declaration, flat
//     "imports":     [ {path,alias,l,c} ... ] }
//
// It should be called inside a forked child process
// the compiler front-end was written for batch use and is not yet robust to
// support half-typed code (it crashes), so the server never calls it
// in-process.
std::string analyze_source(const std::string &path, const std::string &src,
                           bool typecheck);
