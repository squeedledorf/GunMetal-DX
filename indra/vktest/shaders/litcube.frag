#version 450
layout(location = 0) in vec3 vNormalWS;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(vec3(0.4, 0.8, 0.5)); // directional light dir (to light)
    float ndotl = max(dot(N, L), 0.0);
    float ambient = 0.15;
    vec3 baseColor = vec3(0.85, 0.55, 0.25); // warm flat base color
    vec3 color = baseColor * (ambient + ndotl);
    outColor = vec4(color, 1.0);
}
