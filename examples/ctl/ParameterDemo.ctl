// Standard CTL parameter demo for RawNode.
// Plain CTL exposes type/name/default only, so numeric controls are direct
// value fields until host-specific metadata supplies slider ranges.

void main(
    input varying float rIn,
    input varying float gIn,
    input varying float bIn,
    output varying float rOut,
    output varying float gOut,
    output varying float bOut,
    output varying float aOut,
    input varying float aIn = 1.0,
    input uniform float gain = 1.0,
    input uniform int mode = 0,
    input uniform bool enabled = true)
{
    if (!enabled)
    {
        rOut = rIn;
        gOut = gIn;
        bOut = bIn;
    }
    else if (mode == 1)
    {
        rOut = rIn * gain;
        gOut = gIn;
        bOut = bIn;
    }
    else if (mode == 2)
    {
        rOut = rIn;
        gOut = gIn * gain;
        bOut = bIn;
    }
    else if (mode == 3)
    {
        rOut = rIn;
        gOut = gIn;
        bOut = bIn * gain;
    }
    else
    {
        rOut = rIn * gain;
        gOut = gIn * gain;
        bOut = bIn * gain;
    }

    aOut = aIn;
}
