// Please note that this will only work on Unity 5.2 or higher.

#include "AudioPluginUtil.h"

extern float hrtfSrcData[];
extern float reverbmixbuffer[];

namespace InteractivePanning
{
    /* variable */

    // 디버깅 용 콘솔
    FILE* console = NULL;

    //
    enum
    {
        PARAM_AUDIOSRCATTN,
        PARAM_FIXEDVOLUME,
        PARAM_CUSTOMFALLOFF,
        PARAM_NUM
    };

    // 버퍼
    const int HRTFLEN = 512;

    // 보정 계수
    const float GAINCORRECTION = 2.0f;

    class HRTFData
    {
        struct CircleCoeffs
        {
            int numangles;
            float* hrtf;
            float* angles;

            void GetHRTF(UnityComplexNumber* h, float angle, float mix)
            {
                int index1 = 0;
                while (index1 < numangles && angles[index1] < angle)
                    index1++;
                if (index1 > 0)
                    index1--;
                int index2 = (index1 + 1) % numangles;
                float* hrtf1 = hrtf + HRTFLEN * 4 * index1;
                float* hrtf2 = hrtf + HRTFLEN * 4 * index2;
                float f = (angle - angles[index1]) / (angles[index2] - angles[index1]);
                for (int n = 0; n < HRTFLEN * 2; n++)
                {
                    h[n].re += (hrtf1[0] + (hrtf2[0] - hrtf1[0]) * f - h[n].re) * mix;
                    h[n].im += (hrtf1[1] + (hrtf2[1] - hrtf1[1]) * f - h[n].im) * mix;
                    hrtf1 += 2;
                    hrtf2 += 2;
                }
            }
        };

    public:
        CircleCoeffs hrtfChannel[2][14];

    public:

        HRTFData()
        {
            float* p = hrtfSrcData;
            for (int c = 0; c < 2; c++)
            {
                for (int e = 0; e < 14; e++)
                {
                    CircleCoeffs& coeffs = hrtfChannel[c][e];
                    coeffs.numangles = (int)(*p++);
                    coeffs.angles = p;
                    p += coeffs.numangles;
                    coeffs.hrtf = new float[coeffs.numangles * HRTFLEN * 4];
                    float* dst = coeffs.hrtf;
                    UnityComplexNumber h[HRTFLEN * 2];
                    for (int a = 0; a < coeffs.numangles; a++)
                    {
                        memset(h, 0, sizeof(h));
                        for (int n = 0; n < HRTFLEN; n++)
                            h[n + HRTFLEN].re = p[n];
                        p += HRTFLEN;
                        FFT::Forward(h, HRTFLEN * 2, false);
                        for (int n = 0; n < HRTFLEN * 2; n++)
                        {
                            *dst++ = h[n].re;
                            *dst++ = h[n].im;
                        }
                    }
                }
            }
        }

    }; // end internerl class

    static HRTFData sharedData;

    struct InstanceChannel
    {
        UnityComplexNumber h[HRTFLEN * 2];
        UnityComplexNumber x[HRTFLEN * 2];
        UnityComplexNumber y[HRTFLEN * 2];
        float buffer[HRTFLEN * 2];
    };

    struct EffectData
    {
        float p[PARAM_NUM];
        InstanceChannel ch[2];
    };

    static void GetHRTF(int channel, UnityComplexNumber* h, float azimuth, float elevation)
    {
        float e = FastClip(elevation * 0.1f + 4, 0, 12);
        float f = floorf(e);
        int index1 = (int)f;
        if (index1 < 0)
            index1 = 0;
        else if (index1 > 12)
            index1 = 12;
        int index2 = index1 + 1;
        if (index2 > 12)
            index2 = 12;
        sharedData.hrtfChannel[channel][index1].GetHRTF(h, azimuth, 1.0f);
        sharedData.hrtfChannel[channel][index2].GetHRTF(h, azimuth, e - f);
    }

    /* func */

    // API 호환성 및 데이터 사이즈 체크
    inline bool IsHostCompatible(UnityAudioEffectState* state)
    {
        // Somewhat convoluted error checking here because hostapiversion is only supported from SDK version 1.03 (i.e. Unity 5.2) and onwards.
        return
            state->structsize >= sizeof(UnityAudioEffectState) &&
            state->hostapiversion >= UNITY_AUDIO_PLUGIN_API_VERSION;
    }

    // 컨트롤 파라미터 등록
    int InternalRegisterEffectDefinition(UnityAudioEffectDefinition& definition)
    {
        int numparams = PARAM_NUM;
        definition.paramdefs = new UnityAudioParameterDefinition[numparams];
        RegisterParameter(definition, "AudioSrc Attn", "", 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, PARAM_AUDIOSRCATTN, "AudioSource distance attenuation");
        RegisterParameter(definition, "Fixed Volume", "", 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, PARAM_FIXEDVOLUME, "Fixed volume amount");
        RegisterParameter(definition, "Custom Falloff", "", 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, PARAM_CUSTOMFALLOFF, "Custom volume falloff amount (logarithmic)");
        definition.flags |= UnityAudioEffectDefinitionFlags_IsSpatializer;
        return numparams;
    }

    // 거리 감쇄 콜백
    static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        *attenuationOut =
            data->p[PARAM_AUDIOSRCATTN] * attenuationIn +
            data->p[PARAM_FIXEDVOLUME] +
            data->p[PARAM_CUSTOMFALLOFF] * (1.0f / FastMax(1.0f, distanceIn));
        return UNITY_AUDIODSP_OK;
    }

    // 콜백 생성 및 등록
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK CreateCallback(UnityAudioEffectState* state)
    {
        EffectData* effectdata = new EffectData;
        memset(effectdata, 0, sizeof(EffectData));
        state->effectdata = effectdata;
        if (IsHostCompatible(state))
            state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;
        InitParametersFromDefinitions(InternalRegisterEffectDefinition, effectdata->p);

        // 디버깅 용 콘솔 할당
#ifdef _DEBUG
        AllocConsole();
        freopen_s(&console, "CONOUT$", "wb", stdout);
#endif
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index >= PARAM_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        data->p[index] = value;
        return UNITY_AUDIODSP_OK;
    }

    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index >= PARAM_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        if (value != NULL)
            *value = data->p[index];
        if (valuestr != NULL)
            valuestr[0] = 0;
        return UNITY_AUDIODSP_OK;
    }

    int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
    {
        return UNITY_AUDIODSP_OK;
    }

    // 오디오 처리 콜백
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
    {
        // API 버전, NULL 처리, 채널 갯수 처리
        if (inchannels != 2 || outchannels != 2 ||
            !IsHostCompatible(state) || state->spatializerdata == NULL)
        {
            memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
            return UNITY_AUDIODSP_OK;
        }

        EffectData* data = state->GetEffectData<EffectData>();

        static const float kRad2Deg = 180.0f / kPI;

        // 음원, 청자 위치(동차좌표)
        float* listenerMat = state->spatializerdata->listenermatrix;
        float* sourceMat = state->spatializerdata->sourcematrix;

        // Currently we ignore source orientation and only use the position
        float px = sourceMat[12];
        float py = sourceMat[13];
        float pz = sourceMat[14];

        // 방향 벡터
        float dir_x = listenerMat[0] * px + listenerMat[4] * py + listenerMat[8] * pz + listenerMat[12];
        float dir_y = listenerMat[1] * px + listenerMat[5] * py + listenerMat[9] * pz + listenerMat[13];
        float dir_z = listenerMat[2] * px + listenerMat[6] * py + listenerMat[10] * pz + listenerMat[14];

        // 방위 및 고도 계산
        float azimuth_rad = (fabsf(dir_z) < 0.001f) ? 0.0f : atan2f(dir_x, dir_z);
        if (azimuth_rad < 0.0f)
            azimuth_rad += 2.0f * kPI;

        float azimuth_deg = FastClip(azimuth_rad * kRad2Deg, 0.0f, 360.0f);
        float elevation = atan2f(dir_y, sqrtf(dir_x * dir_x + dir_z * dir_z) + 0.001f) * kRad2Deg;

        // 2D 또는 3D 공간 혼합 여부
        float spatialblend = state->spatializerdata->spatialblend;

        // 반향
        float reverbmix = state->spatializerdata->reverbzonemix;
       
        // 반향 버퍼
        float* reverb = reverbmixbuffer;

        // 512씩 샘플씩 돌며
        for (int sampleOffset = 0; sampleOffset < length; sampleOffset += HRTFLEN)
        {
            // 스테레오 채널(L,R)
            for (int c = 0; c < 2; c++)
            {
                // 인터렉티브 패닝 계수
                float interactivePan = ((c == 0) ? cosf((azimuth_rad / 2.0f) - (kPI / 4.0f)) : -sinf((azimuth_rad / 2.0f) - (kPI / 4.0f)));

                // 출력 버퍼, 반향 버퍼 쓰기
                for (int n = 0; n < HRTFLEN; n++)
                {
                    float s = inbuffer[n * 2 + c] * interactivePan;
                    outbuffer[n * 2 + c] = s + s * spatialblend;
                    reverb[n * 2 + c] += outbuffer[n * 2 + c] * reverbmix;
                }
            }

            // 인덱스 증가
            inbuffer += HRTFLEN * 2;
            outbuffer += HRTFLEN * 2;
            reverb += HRTFLEN * 2;
        }

        return UNITY_AUDIODSP_OK;
    }

    /* 리소스 해제 */
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        delete data;
        fclose(console);
        FreeConsole();
        return UNITY_AUDIODSP_OK;
    }

} // end of namespace
