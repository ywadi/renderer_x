#include <rx_asset/mesh_asset.h>

namespace rx::asset {

void AABB::expand(const glm::vec3& p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
}

AABB AABB::unionOf(const AABB& a, const AABB& b) {
    if (!a.isValid()) {
        return b;
    }
    if (!b.isValid()) {
        return a;
    }
    AABB result = a;
    result.expand(b.min);
    result.expand(b.max);
    return result;
}

AABB AABB::transformed(const glm::mat4& m) const {
    if (!isValid()) {
        return *this;
    }

    AABB result;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{
            (i & 1) ? max.x : min.x,
            (i & 2) ? max.y : min.y,
            (i & 4) ? max.z : min.z,
        };
        const glm::vec4 transformed = m * glm::vec4(corner, 1.0F);
        result.expand(glm::vec3(transformed));
    }
    return result;
}

}  // namespace rx::asset
