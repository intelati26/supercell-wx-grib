#version 330 core
smooth in vec4 color;
smooth in vec3 position;

// 1 to light the face by its orientation (filled volume cells), 0 to draw
// the colour as is (ground, grid lines)
uniform float uShade;

layout (location = 0) out vec4 fragColor;

const vec3  kLightDirection = normalize(vec3(0.35f, -0.45f, 0.82f));
const float kAmbient        = 0.55f;

void main()
{
   // Flat face normal from the screen-space change in position: each face
   // of a cell is evenly lit without per-face vertices.
   vec3  normal  = normalize(cross(dFdx(position), dFdy(position)));
   float diffuse = abs(dot(normal, kLightDirection));
   float light   = mix(1.0f, kAmbient + (1.0f - kAmbient) * diffuse, uShade);

   fragColor = vec4(color.rgb * light, color.a);
}
