// Fragment shader for the triangle demo: passes the interpolated
// (solid white) vertex color straight through.
[shader("fragment")]
float4 main(float4 color : COLOR0) : SV_Target
{
    return color;
}
