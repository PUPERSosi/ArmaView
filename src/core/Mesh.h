#pragma once
// Vertex layouts shared by the core (posed attachments, crew) and the renderer, which uploads them as they are.
#include <glm/glm.hpp>

struct MeshVertex
{
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec2 uv1;   // second UV set (#UVSet# 1), used by stages with uvSource "tex1"
    glm::vec4 color; // rgb + blend amount over the base grey (0 = untinted)
};

struct LineVertex
{
    glm::vec3 pos;
    glm::vec4 color;
};
