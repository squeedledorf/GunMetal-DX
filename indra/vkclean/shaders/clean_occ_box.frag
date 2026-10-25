#version 450
// vkclean — occlusion-query proxy box (fragment). No outputs: the pipeline masks
// every colour attachment (colorWriteMask 0); only the depth test / sample count
// matters (VK_QUERY_TYPE_OCCLUSION).
void main()
{
}
