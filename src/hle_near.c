/* hle_near.c — SceNearUtil, the near-field sharing service: not on this machine.
 *
 * "near" let a console leave gifts for the consoles around it. A title
 * that takes part starts the service with a work area, asks it now and
 * then what has turned up, and shuts it down when it leaves its menus.
 * There is no such service here and no neighbours to find, and every
 * function says so: the start fails, the title gives its work area back
 * and carries on without, and whatever it still calls afterwards, the
 * shutdown among them, gets the same answer.
 *
 * The functions arrive by number. The title this was written against
 * imports thirteen of them, from a version of the library whose numbers
 * no table of names carries, so they have none here either; the first is
 * the start, by what the title passes it and does with the answer.
 */

#include "vitarecomp/recomp_rt.h"

#define SCE_ERROR_ERRNO_ENOSYS 0x80010058u

#define ABSENT(nid) void vita_hle_nid_##nid(void) { r0 = SCE_ERROR_ERRNO_ENOSYS; }

ABSENT(A412E9CA)        /* the start: (work area, size, ...) */
ABSENT(E608000B)        /* called once as the title leaves its front end */
ABSENT(1AA394BA)
ABSENT(1AE3EC1C)
ABSENT(49A97D5F)
ABSENT(69EE6FB3)
ABSENT(76C4807C)
ABSENT(8095FDEF)
ABSENT(88540CEC)
ABSENT(9B3F2DCE)
ABSENT(9FB9277B)
ABSENT(BF2BBE1F)
ABSENT(CA742B97)
