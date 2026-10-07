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

#ifndef MP_VIDEO_PROJECTION_H
#define MP_VIDEO_PROJECTION_H

#include <stdint.h>

struct AVSphericalMapping;

// How a video that is a view of a sphere maps the sphere onto the frame,
// as the container says it (MP4 sv3d, Matroska Projection).
enum mp_projection {
    MP_PROJECTION_NONE = 0,             // an ordinary picture, or not told
    MP_PROJECTION_EQUIRECTANGULAR,      // the whole sphere, 360 by 180 degrees
    MP_PROJECTION_HALF_EQUIRECTANGULAR, // the front half, 180 by 180 degrees (VR180)
    MP_PROJECTION_EQUIRECTANGULAR_TILE, // another part of an equirectangular picture
    MP_PROJECTION_CUBEMAP,
    MP_PROJECTION_MESH,                 // by a mesh in the stream, which is not read
};

// The name for the track-list property, NULL for none
const char *mp_projection_name(enum mp_projection projection);

enum mp_projection mp_projection_from_av(const struct AVSphericalMapping *spherical);

// Matroska's ProjectionType: 0 rectangular, 1 equirectangular, 2 cubemap, 3 mesh
enum mp_projection mp_projection_from_mkv(uint64_t projection_type);

#endif
