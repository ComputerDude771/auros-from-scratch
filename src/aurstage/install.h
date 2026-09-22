/* install.h — stage C, in order, with the gate in front of it.
 *
 * This is the file that turns a pile of tested parts into an
 * installer. Everything it calls has its own tests; what is tested
 * HERE is the order, and the order is the product.
 *
 * THE SHAPE, AND THE ONE LINE IN IT THAT MATTERS
 *
 *   the gate      everything answerable with the disk untouched
 *   ---- the only irreversible step in the product is below this ----
 *   phase 4       shrink the filesystem
 *   phase 5/6     write the image by offset, read it back, hash it
 *   phase 7       probe this machine's hardware
 *   phase 8       one sector, then settle and hand over
 *
 * Every question that can be asked before the line is asked before
 * it, even when that costs minutes: a refusal above the line costs
 * the user a restart, and a refusal below it costs them a Windows
 * partition they cannot get back without the recovery payload. That
 * is why the image is hashed off the stick -- three minutes on USB 2 --
 * before anything is touched rather than at the moment the bytes are
 * needed.
 *
 * AND EVERY REFUSAL SAYS WHAT WILL ACTUALLY HAPPEN NEXT. "Nothing has
 * been changed" is true and useless on its own: BootNext is one-shot
 * and was consumed by the boot that is running, so switching the
 * machine on again boots WINDOWS, not the installer. A person who is
 * told only that nothing changed, and who then restarts, concludes
 * the install failed silently.
 */
#ifndef AUROS_INSTALL_H
#define AUROS_INSTALL_H

#include "aurstage.h"

/* Run it. Never returns on success -- it switch_roots into the system
 * it just installed. On any refusal it says why, says what to do, and
 * stops in a way a person can describe over the telephone. */
void install_run(const stage_machine *m);

#endif
