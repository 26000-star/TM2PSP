// Text helpers of the front end's pages (src/app/fe_menu.cpp), shared with the track editor's screen.
#pragma once

#include <string>

#include "nitro.h"

const char *FeS(const char *key);                // a DS string by its key (strings/main), the key when missing
const char *FeL(const char *fr, const char *en); // a port text: French when the strings are French, else English
std::string FeFmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string FeTime(fx32 t);                      // m:ss.cc, "-:--.--" when 0
