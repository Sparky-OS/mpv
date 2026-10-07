#include <libavutil/spherical.h>

#include "test_utils.h"
#include "video/projection.h"

static void test_from_av(void)
{
    AVSphericalMapping s = {0};

    s.projection = AV_SPHERICAL_EQUIRECTANGULAR;
    assert_int_equal(mp_projection_from_av(&s), MP_PROJECTION_EQUIRECTANGULAR);

    s.projection = AV_SPHERICAL_CUBEMAP;
    assert_int_equal(mp_projection_from_av(&s), MP_PROJECTION_CUBEMAP);

    // VR180 as the container writes it: a quarter of the width cut off each side
    s.projection = AV_SPHERICAL_EQUIRECTANGULAR_TILE;
    s.bound_left = s.bound_right = UINT32_C(1) << 30;
    assert_int_equal(mp_projection_from_av(&s), MP_PROJECTION_HALF_EQUIRECTANGULAR);

    // any other part of the picture is only told as a tile
    s.bound_left = UINT32_C(1) << 29;
    assert_int_equal(mp_projection_from_av(&s), MP_PROJECTION_EQUIRECTANGULAR_TILE);
    s.bound_left = s.bound_right = UINT32_C(1) << 30;
    s.bound_top = 1;
    assert_int_equal(mp_projection_from_av(&s), MP_PROJECTION_EQUIRECTANGULAR_TILE);
}

static void test_from_mkv(void)
{
    assert_int_equal(mp_projection_from_mkv(0), MP_PROJECTION_NONE);
    assert_int_equal(mp_projection_from_mkv(1), MP_PROJECTION_EQUIRECTANGULAR);
    assert_int_equal(mp_projection_from_mkv(2), MP_PROJECTION_CUBEMAP);
    assert_int_equal(mp_projection_from_mkv(3), MP_PROJECTION_MESH);
    assert_int_equal(mp_projection_from_mkv(4), MP_PROJECTION_NONE);
}

static void test_names(void)
{
    assert_true(!mp_projection_name(MP_PROJECTION_NONE));
    assert_string_equal(mp_projection_name(MP_PROJECTION_EQUIRECTANGULAR), "equirectangular");
    assert_string_equal(mp_projection_name(MP_PROJECTION_HALF_EQUIRECTANGULAR), "half-equirectangular");
    assert_string_equal(mp_projection_name(MP_PROJECTION_EQUIRECTANGULAR_TILE), "equirectangular-tile");
    assert_string_equal(mp_projection_name(MP_PROJECTION_CUBEMAP), "cubemap");
    assert_string_equal(mp_projection_name(MP_PROJECTION_MESH), "mesh");
}

int main(void)
{
    test_from_av();
    test_from_mkv();
    test_names();
    return 0;
}
