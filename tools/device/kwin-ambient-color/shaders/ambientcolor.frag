#version 140
/*
 * 把窗口离屏纹理按白点适配结果变换到环境白点。
 * colormanagement.glsl 由 KWin 内嵌资源提供，负责源编码与目标编码之间的
 * 线性化与色彩空间转换。
 */
#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;
uniform mat3 adaptation;

in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 tex = texture2D(sampler, texcoord0);

    tex = sourceEncodingToNitsInDestinationColorspace(tex);
    tex.rgb = adaptation * tex.rgb;
    tex *= modulation;

    fragColor = nitsToDestinationEncoding(tex);
}
