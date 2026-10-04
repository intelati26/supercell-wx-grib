#version 330 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec4 aColor;

uniform mat4  uMVPMatrix;
uniform float uVerticalScale;
uniform float uOpacity;

smooth out vec4 color;

void main()
{
   gl_Position = uMVPMatrix *
                 vec4(aPosition.xy, aPosition.z * uVerticalScale, 1.0f);
   color       = vec4(aColor.rgb, aColor.a * uOpacity);
}
