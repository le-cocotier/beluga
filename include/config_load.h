#ifndef CONFIG_LOAD_H_
#define CONFIG_LOAD_H_

/**
 * Loading Lisp config files (config/init.lisp, every package's init.lisp)
 * one top-level form at a time, so a mistake in one form is reported with
 * its file and line and the rest of the file still loads.
 */

/**
 * Reads and evaluates the Lisp file at `path`, form by form. `label` is how
 * the file is named in error messages (e.g. "init.lisp",
 * "packages/git/init.lisp"). A file that can't be opened counts as one
 * error. Returns the number of errors this call recorded.
 */
int configLoadFile(const char *path, const char *label);

/** Records one error message (printf-style), shown by configShowErrors(). */
void configReportError(const char *fmt, ...);

/** Number of errors recorded since startup. */
int configErrorCount(void);

/**
 * Makes the recorded errors visible: the first one goes to the status bar
 * and the full list into a "*config-errors*" buffer. Does nothing when
 * there were none.
 */
void configShowErrors(void);

/** Frees the recorded messages. */
void configErrorsFree(void);

#endif // CONFIG_LOAD_H_
