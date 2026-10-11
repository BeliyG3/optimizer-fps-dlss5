#ifndef PW_RASTER_FXAA_HLSLI
#define PW_RASTER_FXAA_HLSLI
// --fxaa (raster_resolve.hlsl, PW_FXAA=1): FXAA 3.11, quality preset 12 (5 search steps: 1, 1.5, 2, 4, 12 px), subpixel 0.75, edge
// threshold 0.166, minimum 0.0833, ported from Lottes' PC reference to the resolve's compute layout. Included after the resolve's
// targets, so colourTarget, depthTarget and the frame constants are in scope.
//
// What it filters. The raster colour target holds no sky and no fog, so a filter on it alone would blend a silhouette with black:
// every tap is the frame's colour of that pixel (Resolved: the shaded surface or the sky, then fog and the firefly clamp, as the
// resolve writes it), and the result is written in place of the centre. That is the colour a game's own AA sees, fog being part of
// its scene shading; the upscaler, NR and the add-on receive the filtered frame.
//
// Luma. The colour is linear HDR with exposure applied, FXAA's thresholds are tuned for gamma-encoded LDR: the luma is of a
// tonemapped estimate, l / (1 + l) as the present pass's neutral curve, then square-rooted as a cheap sRGB. The colour itself is
// what is blended (bilinear taps, in linear light).

static const float FxaaSubpix=0.75, FxaaEdgeThreshold=0.166, FxaaEdgeThresholdMin=0.0833;
static const float FxaaSearch[4]={1.5,2.0,4.0,12.0}; // preset 12's steps after the first (1.0)

// The finished colour of pixel p (clamped to the target), the same computation as the resolve's body.
float3 Resolved(int2 p)
{
    uint2 q=uint2(clamp(p,int2(0,0),int2(width-1,height-1)));
    float2 ndc=(float2(q)+0.5-float2(jitterX,jitterY))/float2(width,height)*2-1;
    float3 direction=normalize(cameraForward+cameraRight*(ndc.x*aspect*tanHalf)-cameraUp*(ndc.y*tanHalf));
    bool reverse=reverseDepth==1 || reverseDepth==2;
    float z=depthTarget[q];
    float3 sum; float fogDistance, sunVisibility=1;
    if(z!=(reverse ? 0.0 : 1.0)) {
        float4 shaded=colourTarget[q];
        sum=shaded.rgb; sunVisibility=shaded.a;
        fogDistance=min(ViewDepthFromHardware(z,reverse)/dot(direction,cameraForward),300);
    } else {
        sum=max(Sky(direction)*exposure,0);
        if(!all(isfinite(sum))) sum=0;
        fogDistance=300;
    }
    sum=sum*MediumTransmittance(hazeDensity,fogDistance)+RasterFog(direction,fogDistance,sunVisibility)*exposure;
    if(!all(isfinite(sum))) sum=0;
    return sum*min(1,firefly/max(Luminance(sum),1e-8));
}
float FxaaLuma(float3 c)
{
    float l=Luminance(max(c,0));
    return sqrt(l/(1+l));
}
// Bilinear tap at `pos` in pixels (texel centres at +0.5), the hardware filter of the reference. Taps with weight 0 are skipped:
// the search's first steps sit on texel borders.
float3 FxaaColour(float2 pos)
{
    float2 f=pos-0.5, base=floor(f), w=f-base; int2 i=int2(base);
    float3 top=Resolved(i), bottom=Resolved(i+int2(0,1));
    if(w.x>0) { top=lerp(top,Resolved(i+int2(1,0)),w.x); bottom=lerp(bottom,Resolved(i+int2(1,1)),w.x); }
    return w.y>0 ? lerp(top,bottom,w.y) : top;
}
float FxaaLumaAt(float2 pos)
{
    float2 f=pos-0.5, base=floor(f), w=f-base; int2 i=int2(base);
    float top=FxaaLuma(Resolved(i)), bottom=w.y>0 ? FxaaLuma(Resolved(i+int2(0,1))) : 0;
    if(w.x>0) {
        top=lerp(top,FxaaLuma(Resolved(i+int2(1,0))),w.x);
        if(w.y>0) bottom=lerp(bottom,FxaaLuma(Resolved(i+int2(1,1))),w.x);
    }
    return w.y>0 ? lerp(top,bottom,w.y) : top;
}

// `centre` is the pixel's own finished colour (Resolved(pixel)); returns the filtered colour. y grows downward: N is y-1.
float3 Fxaa(uint2 pixel, float3 centre)
{
    const int2 p=int2(pixel);
    float2 posM=float2(pixel)+0.5;
    float lumaM=FxaaLuma(centre);
    float lumaS=FxaaLuma(Resolved(p+int2(0,1))), lumaE=FxaaLuma(Resolved(p+int2(1,0)));
    float lumaN=FxaaLuma(Resolved(p+int2(0,-1))), lumaW=FxaaLuma(Resolved(p+int2(-1,0)));
    float rangeMax=max(max(lumaN,lumaW),max(lumaE,max(lumaS,lumaM)));
    float rangeMin=min(min(lumaN,lumaW),min(lumaE,min(lumaS,lumaM)));
    float range=rangeMax-rangeMin;
    if(range<max(FxaaEdgeThresholdMin,rangeMax*FxaaEdgeThreshold)) return centre; // not an edge
    float lumaNW=FxaaLuma(Resolved(p+int2(-1,-1))), lumaSE=FxaaLuma(Resolved(p+int2(1,1)));
    float lumaNE=FxaaLuma(Resolved(p+int2(1,-1))), lumaSW=FxaaLuma(Resolved(p+int2(-1,1)));
    float lumaNS=lumaN+lumaS, lumaWE=lumaW+lumaE;
    float edgeHorz=abs(-2*lumaW+lumaNW+lumaSW)+abs(-2*lumaM+lumaNS)*2+abs(-2*lumaE+lumaNE+lumaSE);
    float edgeVert=abs(-2*lumaS+lumaSW+lumaSE)+abs(-2*lumaM+lumaWE)*2+abs(-2*lumaN+lumaNW+lumaNE);
    bool horzSpan=edgeHorz>=edgeVert;
    float subpixA=(lumaNS+lumaWE)*2+(lumaNW+lumaSW)+(lumaNE+lumaSE);
    if(!horzSpan) { lumaN=lumaW; lumaS=lumaE; } // the two sides across the edge
    float subpixB=subpixA*(1.0/12.0)-lumaM;
    float gradientN=lumaN-lumaM, gradientS=lumaS-lumaM;
    float lumaNN=lumaN+lumaM, lumaSS=lumaS+lumaM;
    bool pairN=abs(gradientN)>=abs(gradientS);
    float gradient=max(abs(gradientN),abs(gradientS));
    float lengthSign=pairN ? -1 : 1; // pixels from here to the edge's side
    float subpixC=saturate(abs(subpixB)/range);
    float2 posB=posM, offNP=horzSpan ? float2(1,0) : float2(0,1);
    if(horzSpan) posB.y+=lengthSign*0.5; else posB.x+=lengthSign*0.5;
    float2 posN=posB-offNP, posP=posB+offNP;
    if(!pairN) lumaNN=lumaSS;
    float gradientScaled=gradient*0.25;
    float lumaMM=lumaM-lumaNN*0.5;
    bool lumaMLTZero=lumaMM<0;
    float lumaEndN=FxaaLumaAt(posN)-lumaNN*0.5, lumaEndP=FxaaLumaAt(posP)-lumaNN*0.5;
    bool doneN=abs(lumaEndN)>=gradientScaled, doneP=abs(lumaEndP)>=gradientScaled;
    [unroll] for(int i=0;i<4;++i) {
        if(!doneN || !doneP) {
            if(!doneN) { posN-=offNP*FxaaSearch[i]; lumaEndN=FxaaLumaAt(posN)-lumaNN*0.5; doneN=abs(lumaEndN)>=gradientScaled; }
            if(!doneP) { posP+=offNP*FxaaSearch[i]; lumaEndP=FxaaLumaAt(posP)-lumaNN*0.5; doneP=abs(lumaEndP)>=gradientScaled; }
        }
    }
    float dstN=horzSpan ? posM.x-posN.x : posM.y-posN.y;
    float dstP=horzSpan ? posP.x-posM.x : posP.y-posM.y;
    bool goodSpanN=(lumaEndN<0)!=lumaMLTZero, goodSpanP=(lumaEndP<0)!=lumaMLTZero;
    bool directionN=dstN<dstP;
    float dst=min(dstN,dstP);
    bool goodSpan=directionN ? goodSpanN : goodSpanP;
    float subpixF=(-2*subpixC+3)*subpixC*subpixC;
    float pixelOffset=-dst/(dstP+dstN)+0.5;
    float pixelOffsetSubpix=max(goodSpan ? pixelOffset : 0,subpixF*subpixF*FxaaSubpix);
    float2 posFinal=posM;
    if(horzSpan) posFinal.y+=pixelOffsetSubpix*lengthSign; else posFinal.x+=pixelOffsetSubpix*lengthSign;
    return FxaaColour(posFinal);
}
#endif
