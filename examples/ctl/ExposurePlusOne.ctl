// Standard CTL example for RawNode.
// Applies +1 EV (2x) to RGB and passes alpha through unchanged.

void main(
    input varying float rIn,
    input varying float gIn,
    input varying float bIn,
    output varying float rOut,
    output varying float gOut,
    output varying float bOut,
    output varying float aOut,
    input varying float aIn = 1.0)
{
    rOut = rIn * 2.0;
    gOut = gIn * 2.0;
    bOut = bIn * 2.0;
    aOut = aIn;
}
