#pragma once

#include <stdbool.h>

struct mp_image;

// Whether vf_dibr draws pictures of this format (planar, 8 to 16 bit samples)
bool mp_dibr_accepts(int imgfmt);

// Whether what the depth samples of img mean is a malformed Stereo3DDepth mark: the user's mark when there is one,
// else the container's when neither the user's meaning nor the depth map's side data says it (docs/depth-marks.md,
// rule 1: the file is then one picture)
bool mp_dibr_malformed(struct mp_image *img, const char *user_mark, int user_meaning);
