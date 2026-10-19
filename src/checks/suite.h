// Check suite: runs the host test drivers (tm_cfgdump, tm_trackdump, tm_physics) in one process, one dump file per
// check, so the same producers can run on the PSP and their output can be compared byte for byte with the host.
#ifndef TM_CHECKS_SUITE_H
#define TM_CHECKS_SUITE_H

// list: text file, one check per line "<name> <program> <args...>" (python -P tools/host/run_checks.py --list).
// Writes <outdir>/<name>.txt for each check (stdout of the driver, LF line ends) and returns the number of checks
// whose driver failed. log receives one line per check: name, exit code, milliseconds,
// heap bytes in use after the check (heapUsed, may be NULL), files
// left open (FSh_OpenFiles).
int RunSuite(const char *root, const char *list, const char *outdir, const char *log, unsigned (*msNow)(),
             unsigned (*heapUsed)());

#endif
