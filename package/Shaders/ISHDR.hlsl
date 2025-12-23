#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);
#	if defined(DOWNSAMPLE)
SamplerState AdaptSampler : register(s1);
#	elif defined(BLEND)
SamplerState BlendSampler : register(s1);
#	endif
SamplerState AvgSampler : register(s2);

Texture2D<float4> BloomTexture : register(t0);
#	if defined(DOWNSAMPLE)
Texture2D<float4> AdaptTex : register(t1);
#	elif defined(BLEND)
Texture2D<float4> SceneTexture : register(t1);
#	endif
Texture2D<float4> AvgTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float4 Flags : packoffset(c0);
	float4 TimingData : packoffset(c1);
	float4 Param : packoffset(c2);
	float4 Cinematic : packoffset(c3);
	float4 Tint : packoffset(c4);
	float4 Fade : packoffset(c5);
	float4 BlurScale : packoffset(c6);
	float4 BlurOffsets[16] : packoffset(c7);
};

float3 GetTonemapFactorReinhard(float3 luminance)
{
	return (luminance * (luminance * Param.y + 1)) / (luminance + 1);
}

float3 GetTonemapFactorHejlBurgessDawson(float3 luminance)
{
	float3 tmp = max(0, luminance - 0.004);
	return Param.y *
	       pow(((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06), Color::GammaCorrectionValue);
}

#include "Common/DisplayMapping.hlsli"

PS_OUTPUT main(PS_INPUT input){
	PS_OUTPUT psout;

#if defined(DOWNSAMPLE)
	float3 downsampledColor = 0;
	for (int sampleIndex = 0; sampleIndex < DOWNSAMPLE; ++sampleIndex) {
		float2 texCoord = BlurOffsets[sampleIndex].xy * BlurScale.xy + input.TexCoord;
		[branch] if (Flags.x > 0.5)
		{
			texCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(texCoord);
		}
		float3 imageColor = max(0.0, BloomTexture.Sample(ImageSampler, texCoord).xyz);

		#if defined(RGB2LUM)
			imageColor = Color::RGBToLuminance(imageColor);
		#elif (defined(LUM) || defined(LUMCLAMP)) && !defined(DOWNADAPT)
			imageColor = imageColor.x;
		#endif

		downsampledColor += imageColor * BlurOffsets[sampleIndex].z;
	}
	#if defined(DOWNADAPT)
		float2 adaptValue = max(0.001, AdaptTex.Sample(AdaptSampler, input.TexCoord).xy);
		float2 adaptDelta = downsampledColor.xy - adaptValue;
		downsampledColor.xy = sign(adaptDelta) * clamp(abs(Param.wz * adaptDelta), 0.00390625, abs(adaptDelta)) + adaptValue;
	#endif

	psout.Color = float4(downsampledColor, BlurScale.z);
//END DOWNSAMPLE

#elif defined(BLEND)
	float2 uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float3 Scene = SceneTexture.Sample(BlendSampler, uv).xyz;

	float3 bloomColor = 0;
	if (Flags.x > 0.5) {
		bloomColor = BloomTexture.Sample(ImageSampler, uv).xyz;
	} else {
		bloomColor = BloomTexture.Sample(ImageSampler, input.TexCoord.xy).xyz;
	}
	//bloomColor = float3(0,0,0);

	float2 avgValue = AvgTex.Sample(AvgSampler, input.TexCoord.xy).xy;

	// Vanilla tonemapping and post-processing
	if (avgValue.x != 0 && avgValue.y != 0)
		Scene *= avgValue.y / avgValue.x;

	Scene = max(0, Scene);

	float3 OutputColor;
	[branch] if (Param.z > 0.5){
		OutputColor = DisplayMapping::HuePreservingHejlBurgessDawson(Scene, bloomColor);
	} else{
		float SceneLum = Color::RGBToLuminance(Scene);
		OutputColor = Scene * GetTonemapFactorReinhard(SceneLum).x / SceneLum;
		OutputColor += saturate(Param.x - OutputColor) * bloomColor;
	}

	float OutLum = Color::RGBToLuminance(OutputColor);
	OutputColor = lerp(OutLum, OutputColor, Cinematic.x);
	OutputColor = Cinematic.w * lerp(OutputColor, OutLum * Tint.xyz, Tint.w).xyz;

	float3 srgbColor = Scene; //=max(0, lerp(avgValue.x, OutputColor, Cinematic.z));

	#if defined(FADE)
		srgbColor = lerp(srgbColor, Fade.xyz, Fade.w);
	#endif

	psout.Color = float4(FrameBuffer::ToSRGBColor(srgbColor), 1.0);

	#endif

	return psout;
}
#endif
