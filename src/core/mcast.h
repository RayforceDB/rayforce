/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#ifndef RAY_MCAST_H
#define RAY_MCAST_H

#include <rayforce.h>
#include "core/poll.h"

typedef struct ray_mcast ray_mcast_t;

ray_mcast_t* ray_mcast_create(void);
void         ray_mcast_destroy(ray_mcast_t* mc);

ray_t* ray_mcast_sub(ray_poll_t* poll, int64_t handle, ray_t* topic, ray_t* filter);
ray_t* ray_mcast_unsub(ray_poll_t* poll, int64_t handle, ray_t* topic);
ray_t* ray_mcast_pub(ray_poll_t* poll, ray_t* topic, ray_t* payload);
ray_t* ray_mcast_stats(ray_poll_t* poll);
ray_t* ray_mcast_drop(ray_poll_t* poll, int64_t handle);
void   ray_mcast_on_close(ray_poll_t* poll, int64_t handle);

#endif /* RAY_MCAST_H */
