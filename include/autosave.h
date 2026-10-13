#ifndef AUTOSAVE_H_
#define AUTOSAVE_H_

struct buffer_t;

/**
 * Autosave: every AUTOSAVE-INTERVAL seconds (init.lisp, default 15, 0 turns
 * it off), each file buffer with unsaved changes is written to
 * ~/.beluga/autosave/, under its full path with the separators replaced by
 * '!'. Saving the buffer, or killing it after confirming, removes the copy;
 * a crash, a closed terminal or a quit that discards changes leaves it, and
 * opening the file again offers to recover it.
 */

/** Called on every run-loop tick; writes what is due. */
void autosaveTick(void);

/** Writes `buf`'s autosave copy now, whatever the interval. */
void autosaveWrite(struct buffer_t *buf);

/** Removes `buf`'s autosave copy (after a save, or a confirmed kill). */
void autosaveDiscard(struct buffer_t *buf);

/**
 * Called right after a file is loaded into `buf`: if an autosave copy newer
 * than the file exists, asks whether to recover it, and if so replaces the
 * buffer's content with it (the buffer is then modified, not saved).
 */
void autosaveOfferRecovery(struct buffer_t *buf);

#endif // AUTOSAVE_H_
