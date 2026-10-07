/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libavutil/spherical.h>
#include <libavutil/version.h>

#include "projection.h"

const char *mp_projection_name(enum mp_projection projection)
{
    switch (projection) {
    case MP_PROJECTION_EQUIRECTANGULAR: return "equirectangular";
    case MP_PROJECTION_HALF_EQUIRECTANGULAR: return "half-equirectangular";
    case MP_PROJECTION_EQUIRECTANGULAR_TILE: return "equirectangular-tile";
    case MP_PROJECTION_CUBEMAP: return "cubemap";
    case MP_PROJECTION_MESH: return "mesh";
    default: return NULL;
    }
}

enum mp_projection mp_projection_from_av(const AVSphericalMapping *spherical)
{
    switch (spherical->projection) {
    case AV_SPHERICAL_EQUIRECTANGULAR:
        return MP_PROJECTION_EQUIRECTANGULAR;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 23, 100)
    case AV_SPHERICAL_HALF_EQUIRECTANGULAR:
        return MP_PROJECTION_HALF_EQUIRECTANGULAR;
#endif
    case AV_SPHERICAL_EQUIRECTANGULAR_TILE: {
        // The bounds are the parts cut off each side, as fractions of the
        // whole picture in 0.32 fixed point. Cut half of the width and
        // nothing of the height is the front hemisphere of VR180.
        const uint32_t half = UINT32_C(1) << 31;
        if ((uint64_t)spherical->bound_left + spherical->bound_right == half &&
            !spherical->bound_top && !spherical->bound_bottom)
            return MP_PROJECTION_HALF_EQUIRECTANGULAR;
        return MP_PROJECTION_EQUIRECTANGULAR_TILE;
    }
    case AV_SPHERICAL_CUBEMAP:
        return MP_PROJECTION_CUBEMAP;
    default:
        return MP_PROJECTION_NONE;
    }
}

enum mp_projection mp_projection_from_mkv(uint64_t projection_type)
{
    switch (projection_type) {
    case 1: return MP_PROJECTION_EQUIRECTANGULAR;
    case 2: return MP_PROJECTION_CUBEMAP;
    case 3: return MP_PROJECTION_MESH;
    default: return MP_PROJECTION_NONE;
    }
}
