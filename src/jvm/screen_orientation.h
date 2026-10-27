/* SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNARIA_SCREEN_ORIENTATION_H
#define LUNARIA_SCREEN_ORIENTATION_H

/* The emulated device profiles have a portrait-natural panel.  Sensor/user
 * orientations without an actual sensor select the normal orientation. */
static inline int lunaria_orientation_rotation(long orientation)
{
   switch (orientation) {
   case 0: case 6: case 11: return 1; /* landscape */
   case 8: return 3;                  /* reverse landscape */
   case 9: return 2;                  /* reverse portrait */
   default: return 0;
   }
}
#endif
