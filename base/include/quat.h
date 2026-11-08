/* SPDX-License-Identifier: GPL-2.0-only */
/* quat.h — object-side checked quaternion integration helpers. */
#ifndef LITEOS_QUAT_H
#define LITEOS_QUAT_H

#include "base.h"

bool quat_angular_product(const motion_fixed_t q[4],
                          const motion_fixed_t omega[3],
                          motion_fixed_t out[4]);
bool quat_normalize(motion_fixed_t q[4]);

#endif /* LITEOS_QUAT_H */
