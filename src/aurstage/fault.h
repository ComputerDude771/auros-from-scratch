/* fault.h — pulling the plug, on purpose, at a named instant.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT A HACK
 *
 * R4 and R6 are the two risks in this product that cannot be argued
 * away on paper: a single-PC household whose install dies halfway, and
 * a power cut in the one window where the disk is inconsistent. The
 * only honest way to know whether the design survives them is to make
 * them happen -- repeatedly, at every dangerous instant, and get the
 * same answer every time.
 *
 * Doing that by pulling a cord out of a wall gives you one sample per
 * attempt at an instant nobody chose, on one machine, and it is not
 * available to somebody writing this without a laboratory. Doing it by
 * killing a virtual machine from outside at a wall-clock moment gives
 * you a different instant every run, so a failure cannot be
 * reproduced and a pass proves nothing about the instant you cared
 * about.
 *
 * So the installer names its own dangerous instants, and a test run
 * can ask for the machine to stop dead at exactly one of them. Every
 * run is the same run. A failure is reproducible by its name.
 *
 * THE DANGER OF HAVING WRITTEN THIS, taken seriously
 *
 * A flag on the kernel command line that makes the installer halt in
 * the middle of rewriting somebody's partition table is the single
 * worst thing that could end up in a shipped image. So:
 *
 *   - every line of it is behind AURSTAGE_FAULT, which the ordinary
 *     build does not define, so in a release image this file compiles
 *     to nothing at all;
 *   - build/staging greps the finished binary for the flag string and
 *     REFUSES TO PACK an image that contains it, unless the build was
 *     asked for a fault-injection image on purpose;
 *   - and such an image announces itself on every boot, in the first
 *     three lines, so that one cannot be mistaken for a real one.
 *
 * The check is on the linked binary rather than on the source,
 * because the thing that ships is the binary.
 */
#ifndef AUROS_FAULT_H
#define AUROS_FAULT_H

/* Stop the machine dead if `name` is what the kernel command line
 * asked to die at. Returns normally otherwise, and compiles to
 * nothing when AURSTAGE_FAULT is not defined. */
void fault_maybe(const char *name);

/* 1 if this is a fault-injection build. Used by the banner. */
int  fault_build(void);

#endif
