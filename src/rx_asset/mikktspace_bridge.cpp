#include "mikktspace_bridge.h"
#include <mikktspace.h>
#include <cmath>
#include <cstddef>

namespace rx::asset {

namespace {

struct MikkContext {
    std::span<const glm::vec3> positions;
    std::span<const glm::vec3> normals;
    std::span<const glm::vec2> uvs;
    std::vector<glm::vec4> outTangents;
};

int getNumFaces(const SMikkTSpaceContext* ctx) {
    const auto* self = static_cast<const MikkContext*>(ctx->m_pUserData);
    return static_cast<int>(self->positions.size() / 3);
}

int getNumVerticesOfFace(const SMikkTSpaceContext*, const int) { return 3; }

int cornerIndex(int face, int vert) { return face * 3 + vert; }

void getPosition(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const auto* self = static_cast<const MikkContext*>(ctx->m_pUserData);
    const glm::vec3& p = self->positions[static_cast<size_t>(cornerIndex(face, vert))];
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void getNormal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const auto* self = static_cast<const MikkContext*>(ctx->m_pUserData);
    const glm::vec3& n = self->normals[static_cast<size_t>(cornerIndex(face, vert))];
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void getTexCoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const auto* self = static_cast<const MikkContext*>(ctx->m_pUserData);
    const glm::vec2& uv = self->uvs[static_cast<size_t>(cornerIndex(face, vert))];
    out[0] = uv.x;
    out[1] = uv.y;
}

void setTSpaceBasic(const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int face, const int vert) {
    auto* self = static_cast<MikkContext*>(ctx->m_pUserData);
    const size_t idx = static_cast<size_t>(cornerIndex(face, vert));

    float tx = tangent[0];
    float ty = tangent[1];
    float tz = tangent[2];
    float tw = sign;
    // Defensive NaN/Inf clamp -- see mikktspace_bridge.h's own comment.
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz) || !std::isfinite(tw)) {
        tx = 1.0F;
        ty = 0.0F;
        tz = 0.0F;
        tw = 1.0F;
    } else if (tw != 1.0F && tw != -1.0F) {
        // MikkTSpace's own contract is fSign in {-1, +1}; clamp any
        // floating imprecision to the nearest legal value rather than
        // trusting an intermediate value through.
        tw = (tw >= 0.0F) ? 1.0F : -1.0F;
    }

    self->outTangents[idx] = glm::vec4(tx, ty, tz, tw);
}

}  // namespace

std::vector<glm::vec4> generateTangentsDeindexed(std::span<const glm::vec3> positions, std::span<const glm::vec3> normals,
                                                   std::span<const glm::vec2> uvs) {
    const size_t cornerCount = positions.size();
    if (cornerCount == 0 || cornerCount % 3 != 0 || normals.size() != cornerCount || uvs.size() != cornerCount) {
        return {};
    }

    MikkContext context;
    context.positions = positions;
    context.normals = normals;
    context.uvs = uvs;
    context.outTangents.assign(cornerCount, glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));

    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = &getNumFaces;
    iface.m_getNumVerticesOfFace = &getNumVerticesOfFace;
    iface.m_getPosition = &getPosition;
    iface.m_getNormal = &getNormal;
    iface.m_getTexCoord = &getTexCoord;
    iface.m_setTSpaceBasic = &setTSpaceBasic;
    iface.m_setTSpace = nullptr;

    SMikkTSpaceContext mikkCtx{};
    mikkCtx.m_pInterface = &iface;
    mikkCtx.m_pUserData = &context;

    genTangSpaceDefault(&mikkCtx);

    return std::move(context.outTangents);
}

}  // namespace rx::asset
