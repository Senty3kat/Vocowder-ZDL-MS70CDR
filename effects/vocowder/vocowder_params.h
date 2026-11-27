/* Generated from manifest.json. Do not edit by hand. */
#ifndef VOCOWDER_PARAMS_H
#define VOCOWDER_PARAMS_H

#define VOCOWDER_PARAM_COUNT 9

#define VOCOWDER_BANDS_INDEX 0
#define VOCOWDER_BANDS_SLOT 5
#define VOCOWDER_BANDS_TYPE_KNOB 1
#define VOCOWDER_BANDS_UI_MIN 0
#define VOCOWDER_BANDS_UI_MAX 12
#define VOCOWDER_BANDS_UI_DEFAULT 4
#define VOCOWDER_BANDS_AUDIO_MIN 0.0f
#define VOCOWDER_BANDS_AUDIO_MAX 1.0f
#define VOCOWDER_BANDS_DEFAULT_NORM 0.333333333f

#define VOCOWDER_LEVEL_INDEX 1
#define VOCOWDER_LEVEL_SLOT 6
#define VOCOWDER_LEVEL_TYPE_KNOB 1
#define VOCOWDER_LEVEL_UI_MIN 0
#define VOCOWDER_LEVEL_UI_MAX 100
#define VOCOWDER_LEVEL_UI_DEFAULT 50
#define VOCOWDER_LEVEL_AUDIO_MIN 0.0f
#define VOCOWDER_LEVEL_AUDIO_MAX 1.0f
#define VOCOWDER_LEVEL_DEFAULT_NORM 0.5f

#define VOCOWDER_MIX_INDEX 2
#define VOCOWDER_MIX_SLOT 7
#define VOCOWDER_MIX_TYPE_KNOB 1
#define VOCOWDER_MIX_UI_MIN 0
#define VOCOWDER_MIX_UI_MAX 100
#define VOCOWDER_MIX_UI_DEFAULT 100
#define VOCOWDER_MIX_AUDIO_MIN 0.0f
#define VOCOWDER_MIX_AUDIO_MAX 1.0f
#define VOCOWDER_MIX_DEFAULT_NORM 1.0f

#define VOCOWDER_LOCUT_INDEX 3
#define VOCOWDER_LOCUT_SLOT 8
#define VOCOWDER_LOCUT_TYPE_KNOB 1
#define VOCOWDER_LOCUT_UI_MIN 0
#define VOCOWDER_LOCUT_UI_MAX 100
#define VOCOWDER_LOCUT_UI_DEFAULT 30
#define VOCOWDER_LOCUT_AUDIO_MIN 0.0f
#define VOCOWDER_LOCUT_AUDIO_MAX 1.0f
#define VOCOWDER_LOCUT_DEFAULT_NORM 0.3f

#define VOCOWDER_HICUT_INDEX 4
#define VOCOWDER_HICUT_SLOT 9
#define VOCOWDER_HICUT_TYPE_KNOB 1
#define VOCOWDER_HICUT_UI_MIN 0
#define VOCOWDER_HICUT_UI_MAX 100
#define VOCOWDER_HICUT_UI_DEFAULT 0
#define VOCOWDER_HICUT_AUDIO_MIN 0.0f
#define VOCOWDER_HICUT_AUDIO_MAX 1.0f
#define VOCOWDER_HICUT_DEFAULT_NORM 0.0f

#define VOCOWDER_NOISE_INDEX 5
#define VOCOWDER_NOISE_SLOT 10
#define VOCOWDER_NOISE_TYPE_KNOB 1
#define VOCOWDER_NOISE_UI_MIN 0
#define VOCOWDER_NOISE_UI_MAX 100
#define VOCOWDER_NOISE_UI_DEFAULT 0
#define VOCOWDER_NOISE_AUDIO_MIN 0.0f
#define VOCOWDER_NOISE_AUDIO_MAX 1.0f
#define VOCOWDER_NOISE_DEFAULT_NORM 0.0f

#define VOCOWDER_NTYPE_INDEX 6
#define VOCOWDER_NTYPE_SLOT 11
#define VOCOWDER_NTYPE_TYPE_KNOB 1
#define VOCOWDER_NTYPE_UI_MIN 0
#define VOCOWDER_NTYPE_UI_MAX 8
#define VOCOWDER_NTYPE_UI_DEFAULT 1
#define VOCOWDER_NTYPE_AUDIO_MIN 0.0f
#define VOCOWDER_NTYPE_AUDIO_MAX 1.0f
#define VOCOWDER_NTYPE_DEFAULT_NORM 0.125f

#define VOCOWDER_OUT_INDEX 7
#define VOCOWDER_OUT_SLOT 12
#define VOCOWDER_OUT_TYPE_KNOB 1
#define VOCOWDER_OUT_UI_MIN 0
#define VOCOWDER_OUT_UI_MAX 101
#define VOCOWDER_OUT_UI_DEFAULT 0
#define VOCOWDER_OUT_AUDIO_MIN 0.0f
#define VOCOWDER_OUT_AUDIO_MAX 1.0f
#define VOCOWDER_OUT_DEFAULT_NORM 0.0f

#define VOCOWDER_GAIN_INDEX 8
#define VOCOWDER_GAIN_SLOT 13
#define VOCOWDER_GAIN_TYPE_KNOB 1
#define VOCOWDER_GAIN_UI_MIN 0
#define VOCOWDER_GAIN_UI_MAX 100
#define VOCOWDER_GAIN_UI_DEFAULT 50
#define VOCOWDER_GAIN_AUDIO_MIN 0.0f
#define VOCOWDER_GAIN_AUDIO_MAX 1.0f
#define VOCOWDER_GAIN_DEFAULT_NORM 0.5f
int ZDL_GetLabel_0(unsigned int value, char *out) {
    const char *s; int i;
    if (value <= 0u) s = "4";
    else if (value <= 1u) s = "6";
    else if (value <= 2u) s = "8";
    else if (value <= 3u) s = "10";
    else if (value <= 4u) s = "12";
    else if (value <= 5u) s = "16";
    else if (value <= 6u) s = "20";
    else if (value <= 7u) s = "24";
    else if (value <= 8u) s = "32";
    else if (value <= 9u) s = "40";
    else if (value <= 10u) s = "48";
    else if (value <= 11u) s = "64";
    else s = "80";
    for (i=0; i<7; ++i) { out[i]=s[i]; if (!s[i]) return i; }
    out[7]=0; return 7;
}
int ZDL_GetLabel_6(unsigned int value, char *out) {
    const char *s; int i;
    if (value <= 0u) s = "White";
    else if (value <= 1u) s = "Pink";
    else if (value <= 2u) s = "Brown";
    else if (value <= 3u) s = "Green";
    else if (value <= 4u) s = "Blue";
    else if (value <= 5u) s = "Crush";
    else if (value <= 6u) s = "Freez";
    else if (value <= 7u) s = "FrzL";
    else s = "FRev";
    for (i=0; i<7; ++i) { out[i]=s[i]; if (!s[i]) return i; }
    out[7]=0; return 7;
}

#endif /* VOCOWDER_PARAMS_H */
