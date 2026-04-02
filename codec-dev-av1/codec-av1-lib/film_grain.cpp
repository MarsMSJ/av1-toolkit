#include "film_grain.hpp"
#include <algorithm>

namespace av1 {

// Sourced from aom_check_grain_params_equiv in aom/aom_dsp/grain_params.h
bool check_grain_params_equiv(const FilmGrainParams& pa, const FilmGrainParams& pb) {
    if (pa.apply_grain != pb.apply_grain) return false;
    if (pa.num_y_points != pb.num_y_points) return false;
    if (pa.scaling_points_y != pb.scaling_points_y) return false;
    
    if (pa.num_cb_points != pb.num_cb_points) return false;
    if (pa.scaling_points_cb != pb.scaling_points_cb) return false;
    
    if (pa.num_cr_points != pb.num_cr_points) return false;
    if (pa.scaling_points_cr != pb.scaling_points_cr) return false;

    if (pa.scaling_shift != pb.scaling_shift) return false;
    if (pa.ar_coeff_lag != pb.ar_coeff_lag) return false;

    if (pa.ar_coeffs_y != pb.ar_coeffs_y) return false;
    if (pa.ar_coeffs_cb != pb.ar_coeffs_cb) return false;
    if (pa.ar_coeffs_cr != pb.ar_coeffs_cr) return false;

    if (pa.ar_coeff_shift != pb.ar_coeff_shift) return false;
    if (pa.cb_mult != pb.cb_mult) return false;
    if (pa.cb_luma_mult != pb.cb_luma_mult) return false;
    if (pa.cb_offset != pb.cb_offset) return false;
    if (pa.cr_mult != pb.cr_mult) return false;
    if (pa.cr_luma_mult != pb.cr_luma_mult) return false;
    if (pa.cr_offset != pb.cr_offset) return false;
    
    if (pa.overlap_flag != pb.overlap_flag) return false;
    if (pa.clip_to_restricted_range != pb.clip_to_restricted_range) return false;
    if (pa.bit_depth != pb.bit_depth) return false;
    if (pa.chroma_scaling_from_luma != pb.chroma_scaling_from_luma) return false;
    if (pa.grain_scale_shift != pb.grain_scale_shift) return false;
    
    return true;
}

// Sourced from aom/av1/decoder/grain_synthesis.c

// Specified in Section 7.18.3.1 (get_random_number process) of AV1 specification
int FilmGrainSynthesizer::get_random_number(int bits) {
    uint16_t bit = ((random_register_ >> 0) ^ (random_register_ >> 1) ^
                    (random_register_ >> 3) ^ (random_register_ >> 12)) & 1;
    random_register_ = (random_register_ >> 1) | (bit << 15);
    return (random_register_ >> (16 - bits)) & ((1 << bits) - 1);
}

void FilmGrainSynthesizer::init_random_generator(int luma_line, uint16_t seed) {
    uint16_t msb = (seed >> 8) & 255;
    uint16_t lsb = seed & 255;

    random_register_ = (msb << 8) + lsb;

    int luma_num = luma_line >> 5;

    random_register_ ^= ((luma_num * 37 + 178) & 255) << 8;
    random_register_ ^= ((luma_num * 173 + 105) & 255);
}

const int FilmGrainSynthesizer::gaussian_sequence[2048] = {
  56,    568,   -180,  172,   124,   -84,   172,   -64,   -900,  24,   820,
  224,   1248,  996,   272,   -8,    -916,  -388,  -732,  -104,  -188, 800,
  112,   -652,  -320,  -376,  140,   -252,  492,   -168,  44,    -788, 588,
  -584,  500,   -228,  12,    680,   272,   -476,  972,   -100,  652,  368,
  432,   -196,  -720,  -192,  1000,  -332,  652,   -136,  -552,  -604, -4,
  192,   -220,  -136,  1000,  -52,   372,   -96,   -624,  124,   -24,  396,
  540,   -12,   -104,  640,   464,   244,   -208,  -84,   368,   -528, -740,
  248,   -968,  -848,  608,   376,   -60,   -292,  -40,   -156,  252,  -292,
  248,   224,   -280,  400,   -244,  244,   -60,   76,    -80,   212,  532,
  340,   128,   -36,   824,   -352,  -60,   -264,  -96,   -612,  416,  -704,
  220,   -204,  640,   -160,  1220,  -408,  900,   336,   20,    -336, -96,
  -792,  304,   48,    -28,   -1232, -1172, -448,  104,   -292,  -520, 244,
  60,    -948,  0,     -708,  268,   108,   356,   -548,  488,   -344, -136,
  488,   -196,  -224,  656,   -236,  -1128, 60,    4,     140,   276,  -676,
  -376,  168,   -108,  464,   8,     564,   64,    240,   308,   -300, -400,
  -456,  -136,  56,    120,   -408,  -116,  436,   504,   -232,  328,  844,
  -164,  -84,   784,   -168,  232,   -224,  348,   -376,  128,   568,  96,
  -1244, -288,  276,   848,   832,   -360,  656,   464,   -384,  -332, -356,
  728,   -388,  160,   -192,  468,   296,   224,   140,   -776,  -100, 280,
  4,     196,   44,    -36,   -648,  932,   16,    1428,  28,    528,  808,
  772,   20,    268,   88,    -332,  -284,  124,   -384,  -448,  208,  -228,
  -1044, -328,  660,   380,   -148,  -300,  588,   240,   540,   28,   136,
  -88,   -436,  256,   296,   -1000, 1400,  0,     -48,   1056,  -136, 264,
  -528,  -1108, 632,   -484,  -592,  -344,  796,   124,   -668,  -768, 388,
  1296,  -232,  -188,  -200,  -288,  -4,    308,   100,   -168,  256,  -500,
  204,   -508,  648,   -136,  372,   -272,  -120,  -1004, -552,  -548, -384,
  548,   -296,  428,   -108,  -8,    -912,  -324,  -224,  -88,   -112, -220,
  -100,  996,   -796,  548,   360,   -216,  180,   428,   -200,  -212, 148,
  96,    148,   284,   216,   -412,  -320,  120,   -300,  -384,  -604, -572,
  -332,  -8,    -180,  -176,  696,   116,   -88,   628,   76,    44,   -516,
  240,   -208,  -40,   100,   -592,  344,   -308,  -452,  -228,  20,   916,
  -1752, -136,  -340,  -804,  140,   40,    512,   340,   248,   184,  -492,
  896,   -156,  932,   -628,  328,   -688,  -448,  -616,  -752,  -100, 560,
  -1020, 180,   -800,  -64,   76,    576,   1068,  396,   660,   552,  -108,
  -28,   320,   -628,  312,   -92,   -92,   -472,  268,   16,    560,  516,
  -672,  -52,   492,   -100,  260,   384,   284,   292,   304,   -148, 88,
  -152,  1012,  1064,  -228,  164,   -376,  -684,  592,   -392,  156,  196,
  -524,  -64,   -884,  160,   -176,  636,   648,   404,   -396,  -436, 864,
  424,   -728,  988,   -604,  904,   -592,  296,   -224,  536,   -176, -920,
  436,   -48,   1176,  -884,  416,   -776,  -824,  -884,  524,   -548, -564,
  -68,   -164,  -96,   692,   364,   -692,  -1012, -68,   260,   -480, 876,
  -1116, 452,   -332,  -352,  892,   -1088, 1220,  -676,  12,    -292, 244,
  496,   372,   -32,   280,   200,   112,   -440,  -96,   24,    -644, -184,
  56,    -432,  224,   -980,  272,   -260,  144,   -436,  420,   356,  364,
  -528,  76,    172,   -744,  -368,  404,   -752,  -416,  684,   -688, 72,
  540,   416,   92,    444,   480,   -72,   -1416, 164,   -1172, -68,  24,
  424,   264,   1040,  128,   -912,  -524,  -356,  64,    876,   -12,  4,
  -88,   532,   272,   -524,  320,   276,   -508,  940,   24,    -400, -120,
  756,   60,    236,   -412,  100,   376,   -484,  400,   -100,  -740, -108,
  -260,  328,   -268,  224,   -200,  -416,  184,   -604,  -564,  -20,  296,
  60,    892,   -888,  60,    164,   68,    -760,  216,   -296,  904,  -336,
  -28,   404,   -356,  -568,  -208,  -1480, -512,  296,   328,   -360, -164,
  -1560, -776,  1156,  -428,  164,   -504,  -112,  120,   -216,  -148, -264,
  308,   32,    64,    -72,   72,    116,   176,   -64,   -272,  460,  -536,
  -784,  -280,  348,   108,   -752,  -132,  524,   -540,  -776,  116,  -296,
  -1196, -288,  -560,  1040,  -472,  116,   -848,  -1116, 116,   636,  696,
  284,   -176,  1016,  204,   -864,  -648,  -248,  356,   972,   -584, -204,
  264,   880,   528,   -24,   -184,  116,   448,   -144,  828,   524,  212,
  -212,  52,    12,    200,   268,   -488,  -404,  -880,  824,   -672, -40,
  908,   -248,  500,   716,   -576,  492,   -576,  16,    720,   -108, 384,
  124,   344,   280,   576,   -500,  252,   104,   -308,  196,   -188, -8,
  1268,  296,   1032,  -1196, 436,   316,   372,   -432,  -200,  -660, 704,
  -224,  596,   -132,  268,   32,    -452,  884,   104,   -1008, 424,  -1348,
  -280,  4,     -1168, 368,   476,   696,   300,   -8,    24,    180,  -592,
  -196,  388,   304,   500,   724,   -160,  244,   -84,   272,   -256, -420,
  320,   208,   -144,  -156,  156,   364,   452,   28,    540,   316,  220,
  -644,  -248,  464,   72,    360,   32,    -388,  496,   -680,  -48,  208,
  -116,  -408,  60,    -604,  -392,  548,   -840,  784,   -460,  656,  -544,
  -388,  -264,  908,   -800,  -628,  -612,  -568,  572,   -220,  164,  288,
  -16,   -308,  308,   -112,  -636,  -760,  280,   -668,  432,   364,  240,
  -196,  604,   340,   384,   196,   592,   -44,   -500,  432,   -580, -132,
  636,   -76,   392,   4,     -412,  540,   508,   328,   -356,  -36,  16,
  -220,  -64,   -248,  -60,   24,    -192,  368,   1040,  92,    -24,  -1044,
  -32,   40,    104,   148,   192,   -136,  -520,  56,    -816,  -224, 732,
  392,   356,   212,   -80,   -424,  -1008, -324,  588,   -1496, 576,  460,
  -816,  -848,  56,    -580,  -92,   -1372, -112,  -496,  200,   364,  52,
  -140,  48,    -48,   -60,   84,    72,    40,    132,   -356,  -268, -104,
  -284,  -404,  732,   -520,  164,   -304,  -540,  120,   328,   -76,  -460,
  756,   388,   588,   236,   -436,  -72,   -176,  -404,  -316,  -148, 716,
  -604,  404,   -72,   -88,   -888,  -68,   944,   88,    -220,  -344, 960,
  472,   460,   -232,  704,   120,   832,   -228,  692,   -508,  132,  -476,
  844,   -748,  -364,  -44,   1116,  -1104, -1056, 76,    428,   552,  -692,
  60,    356,   96,    -384,  -188,  -612,  -576,  736,   508,   892,  352,
  -1132, 504,   -24,   -352,  324,   332,   -600,  -312,  292,   508,  -144,
  -8,    484,   48,    284,   -260,  -240,  256,   -100,  -292,  -204, -44,
  472,   -204,  908,   -188,  -1000, -256,  92,    1164,  -392,  564,  356,
  652,   -28,   -884,  256,   484,   -192,  760,   -176,  376,   -524, -452,
  -436,  860,   -736,  212,   124,   504,   -476,  468,   76,    -472, 552,
  -692,  -944,  -620,  740,   -240,  400,   132,   20,    192,   -196, 264,
  -668,  -1012, -60,   296,   -316,  -828,  76,    -156,  284,   -768, -448,
  -832,  148,   248,   652,   616,   1236,  288,   -328,  -400,  -124, 588,
  220,   520,   -696,  1032,  768,   -740,  -92,   -272,  296,   448,  -464,
  412,   -200,  392,   440,   -200,  264,   -152,  -260,  320,   1032, 216,
  320,   -8,    -64,   156,   -1016, 1084,  1172,  536,   484,   -432, 132,
  372,   -52,   -256,  84,    116,   -352,  48,    116,   304,   -384, 412,
  924,   -300,  528,   628,   180,   648,   44,    -980,  -220,  1320, 48,
  332,   748,   524,   -268,  -720,  540,   -276,  564,   -344,  -208, -196,
  436,   896,   88,    -392,  132,   80,    -964,  -288,  568,   56,   -48,
  -456,  888,   8,     552,   -156,  -292,  948,   288,   128,   -716, -292,
  1192,  -152,  876,   352,   -600,  -260,  -812,  -468,  -28,   -120, -32,
  -44,   1284,  496,   192,   464,   312,   -76,   -516,  -380,  -456, -1012,
  -48,   308,   -156,  36,    492,   -156,  -808,  188,   1652,  68,   -120,
  -116,  316,   160,   -140,  352,   808,   -416,  592,   316,   -480, 56,
  528,   -204,  -568,  372,   -232,  752,   -344,  744,   -4,    324,  -416,
  -600,  768,   268,   -248,  -88,   -132,  -420,  -432,  80,    -288, 404,
  -316,  -1216, -588,  520,   -108,  92,    -320,  368,   -480,  -216, -92,
  1688,  -300,  180,   1020,  -176,  820,   -68,   -228,  -260,  436,  -904,
  20,    40,    -508,  440,   -736,  312,   332,   204,   760,   -372, 728,
  96,    -20,   -632,  -520,  -560,  336,   1076,  -64,   -532,  776,  584,
  192,   396,   -728,  -520,  276,   -188,  80,    -52,   -612,  -252, -48,
  648,   212,   -688,  228,   -52,   -260,  428,   -412,  -272,  -404, 180,
  816,   -796,  48,    152,   484,   -88,   -216,  988,   696,   188,  -528,
  648,   -116,  -180,  316,   476,   12,    -564,  96,    476,   -252, -364,
  -376,  -392,  556,   -256,  -576,  260,   -352,  120,   -16,   -136, -260,
  -492,  72,    556,   660,   580,   616,   772,   436,   424,   -32,  -324,
  -1268, 416,   -324,  -80,   920,   160,   228,   724,   32,    -516, 64,
  384,   68,    -128,  136,   240,   248,   -204,  -68,   252,   -932, -120,
  -480,  -628,  -84,   192,   852,   -404,  -288,  -132,  204,   100,  168,
  -68,   -196,  -868,  460,   1080,  380,   -80,   244,   0,     484,  -888,
  64,    184,   352,   600,   460,   164,   604,   -196,  320,   -64,  588,
  -184,  228,   12,    372,   48,    -848,  -344,  224,   208,   -200, 484,
  128,   -20,   272,   -468,  -840,  384,   256,   -720,  -520,  -464, -580,
  112,   -120,  644,   -356,  -208,  -608,  -528,  704,   560,   -424, 392,
  828,   40,    84,    200,   -152,  0,     -144,  584,   280,   -120, 80,
  -556,  -972,  -196,  -472,  724,   80,    168,   -32,   88,    160,  -688,
  0,     160,   356,   372,   -776,  740,   -128,  676,   -248,  -480, 4,
  -364,  96,    544,   232,   -1032, 956,   236,   356,   20,    -40,  300,
  24,    -676,  -596,  132,   1120,  -104,  532,   -1096, 568,   648,  444,
  508,   380,   188,   -376,  -604,  1488,  424,   24,    756,   -220, -192,
  716,   120,   920,   688,   168,   44,    -460,  568,   284,   1144, 1160,
  600,   424,   888,   656,   -356,  -320,  220,   316,   -176,  -724, -188,
  -816,  -628,  -348,  -228,  -380,  1012,  -452,  -660,  736,   928,  404,
  -696,  -72,   -268,  -892,  128,   184,   -344,  -780,  360,   336,  400,
  344,   428,   548,   -112,  136,   -228,  -216,  -820,  -516,  340,  92,
  -136,  116,   -300,  376,   -244,  100,   -316,  -520,  -284,  -12,  824,
  164,   -548,  -180,  -128,  116,   -924,  -828,  268,   -368,  -580, 620,
  192,   160,   0,     -1676, 1068,  424,   -56,   -360,  468,   -156, 720,
  288,   -528,  556,   -364,  548,   -148,  504,   316,   152,   -648, -620,
  -684,  -24,   -376,  -384,  -108,  -920,  -1032, 768,   180,   -264, -508,
  -1268, -260,  -60,   300,   -240,  988,   724,   -376,  -576,  -212, -736,
  556,   192,   1092,  -620,  -880,  376,   -56,   -4,    -216,  -32,  836,
  268,   396,   1332,  864,   -600,  100,   56,    -412,  -92,   356,  180,
  884,   -468,  -436,  292,   -388,  -804,  -704,  -840,  368,   -348, 140,
  -724,  1536,  940,   372,   112,   -372,  436,   -480,  1136,  296,  -32,
  -228,  132,   -48,   -220,  868,   -1016, -60,   -1044, -464,  328,  916,
  244,   12,    -736,  -296,  360,   468,   -376,  -108,  -92,   788,  368,
  -56,   544,   400,   -672,  -420,  728,   16,    320,   44,    -284, -380,
  -796,  488,   132,   204,   -596,  -372,  88,    -152,  -908,  -636, -572,
  -624,  -116,  -692,  -200,  -56,   276,   -88,   484,   -324,  948,  864,
  1000,  -456,  -184,  -276,  292,   -296,  156,   676,   320,   160,  908,
  -84,   -1236, -288,  -116,  260,   -372,  -644,  732,   -756,  -96,  84,
  344,   -520,  348,   -688,  240,   -84,   216,   -1044, -136,  -676, -396,
  -1500, 960,   -40,   176,   168,   1516,  420,   -504,  -344,  -364, -360,
  1216,  -940,  -380,  -212,  252,   -660,  -708,  484,   -444,  -152, 928,
  -120,  1112,  476,   -260,  560,   -148,  -344,  108,   -196,  228,  -288,
  504,   560,   -328,  -88,   288,   -1008, 460,   -228,  468,   -836, -196,
  76,    388,   232,   412,   -1168, -716,  -644,  756,   -172,  -356, -504,
  116,   432,   528,   48,    476,   -168,  -608,  448,   160,   -532, -272,
  28,    -676,  -12,   828,   980,   456,   520,   104,   -104,  256,  -344,
  -4,    -28,   -368,  -52,   -524,  -572,  -556,  -200,  768,   1124, -208,
  -512,  176,   232,   248,   -148,  -888,  604,   -600,  -304,  804,  -156,
  -212,  488,   -192,  -804,  -256,  368,   -360,  -916,  -328,  228,  -240,
  -448,  -472,  856,   -556,  -364,  572,   -12,   -156,  -368,  -340, 432,
  252,   -752,  -152,  288,   268,   -580,  -848,  -592,  108,   -76,  244,
  312,   -716,  592,   -80,   436,   360,   4,     -248,  160,   516,  584,
  732,   44,    -468,  -280,  -292,  -156,  -588,  28,    308,   912,  24,
  124,   156,   180,   -252,  944,   -924,  -772,  -520,  -428,  -624, 300,
  -212,  -1144, 32,    -724,  800,   -1128, -212,  -1288, -848,  180,  -416,
  440,   192,   -576,  -792,  -76,   -1080, 80,    -532,  -352,  -132, 380,
  -820,  148,   1112,  128,   164,   456,   700,   -924,  144,   -668, -384,
  648,   -832,  508,   552,   -52,   -100,  -656,  208,   -568,  748,  -88,
  680,   232,   300,   192,   -408,  -1012, -152,  -252,  -268,  272,  -876,
  -664,  -648,  -332,  -136,  16,    12,    1152,  -28,   332,   -536, 320,
  -672,  -460,  -316,  532,   -260,  228,   -40,   1052,  -816,  180,  88,
  -496,  -556,  -672,  -368,  428,   92,    356,   404,   -408,  252,  196,
  -176,  -556,  792,   268,   32,    372,   40,    96,    -332,  328,  120,
  372,   -900,  -40,   472,   -264,  -592,  952,   128,   656,   112,  664,
  -232,  420,   4,     -344,  -464,  556,   244,   -416,  -32,   252,  0,
  -412,  188,   -696,  508,   -476,  324,   -1096, 656,   -312,  560,  264,
  -136,  304,   160,   -64,   -580,  248,   336,   -720,  560,   -348, -288,
  -276,  -196,  -500,  852,   -544,  -236,  -1128, -992,  -776,  116,  56,
  52,    860,   884,   212,   -12,   168,   1020,  512,   -552,  924,  -148,
  716,   188,   164,   -340,  -520,  -184,  880,   -152,  -680,  -208, -1156,
  -300,  -528,  -472,  364,   100,   -744,  -1056, -32,   540,   280,  144,
  -676,  -32,   -232,  -280,  -224,  96,    568,   -76,   172,   148,  148,
  104,   32,    -296,  -32,   788,   -80,   32,    -16,   280,   288,  944,
  428,   -484
};

static inline int clamp(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// Specified in Section 7.18.3.2 (init_scaling_function process) of AV1 specification.
// scaling_points is a pointer to rows of {x, y} pairs — call with .data() on
// either the 14-point (luma) or 10-point (chroma) std::array. Same logic, same
// 256-entry LUT, works for both 8-bit and 10-bit (scale_LUT handles interpolation
// at lookup time for 10-bit pixel values).
void FilmGrainSynthesizer::init_scaling_function(
    const std::array<int, 2>* pts, int num_points, std::array<int, 256>& lut) {
  if (num_points == 0) return;

  for (int i = 0; i < pts[0][0]; i++)
    lut[i] = pts[0][1];

  for (int p = 0; p < num_points - 1; p++) {
    int delta_y = pts[p + 1][1] - pts[p][1];
    int delta_x = pts[p + 1][0] - pts[p][0];
    int64_t delta = delta_y * ((65536 + (delta_x >> 1)) / delta_x);
    for (int x = 0; x < delta_x; x++)
      lut[pts[p][0] + x] = pts[p][1] + (int)((x * delta + 32768) >> 16);
  }

  for (int i = pts[num_points - 1][0]; i < 256; i++)
    lut[i] = pts[num_points - 1][1];
}

// Specified in Section 7.18.3.3 (scale_lut process) of AV1 specification
int FilmGrainSynthesizer::scale_LUT(const std::array<int, 256>& scaling_lut,
                                    int index, int bit_depth) {
  int x = index >> (bit_depth - 8);
  if (!(bit_depth - 8) || x == 255)
    return scaling_lut[x];
  else
    return scaling_lut[x] + (((scaling_lut[x + 1] - scaling_lut[x]) *
                               (index & ((1 << (bit_depth - 8)) - 1)) +
                               (1 << (bit_depth - 9))) >>
                              (bit_depth - 8));
}

// ---------------------------------------------------------------------------
// Grain template generation — Section 7.18.3.2
// ---------------------------------------------------------------------------

// Fills luma_grain_block with scaled Gaussian noise, then applies the AR filter.
// The block is (luma_block_size_y x luma_block_size_x) with stride luma_grain_stride.
// Padding of top_pad/left_pad rows/cols is needed to stabilise the AR process;
// only the inner 64x64 region (2 x luma_subblock_size) is used for adding grain.
void FilmGrainSynthesizer::generate_luma_grain_block(
    const FilmGrainParams& params,
    const std::vector<std::array<int, 3>>& pred_pos_luma,
    int* luma_grain_block,
    int luma_block_size_y, int luma_block_size_x, int luma_grain_stride,
    int left_pad, int top_pad, int right_pad, int bottom_pad) {
  if (params.num_y_points == 0) {
    std::fill(luma_grain_block,
              luma_grain_block + luma_block_size_y * luma_grain_stride, 0);
    return;
  }

  const int bit_depth = params.bit_depth;
  // gauss_sec_shift scales the 12-bit Gaussian values to the target bit depth.
  // For 8-bit with grain_scale_shift=0: shift = 12 - 8 + 0 = 4.
  const int gauss_sec_shift = 12 - bit_depth + params.grain_scale_shift;
  const int num_pos = static_cast<int>(pred_pos_luma.size());
  const int rounding_offset = (1 << (params.ar_coeff_shift - 1));

  // Fill entire block with scaled Gaussian samples
  for (int i = 0; i < luma_block_size_y; i++)
    for (int j = 0; j < luma_block_size_x; j++)
      luma_grain_block[i * luma_grain_stride + j] =
          (gaussian_sequence[get_random_number(gauss_bits)] +
           ((1 << gauss_sec_shift) >> 1)) >>
          gauss_sec_shift;

  // AR filter: each valid sample is updated using its causal neighbourhood.
  // The padding rows/cols guarantee that all reference positions are in bounds.
  for (int i = top_pad; i < luma_block_size_y - bottom_pad; i++) {
    for (int j = left_pad; j < luma_block_size_x - right_pad; j++) {
      int wsum = 0;
      for (int pos = 0; pos < num_pos; pos++) {
        wsum += params.ar_coeffs_y[pos] *
                luma_grain_block[(i + pred_pos_luma[pos][0]) * luma_grain_stride +
                                  j + pred_pos_luma[pos][1]];
      }
      luma_grain_block[i * luma_grain_stride + j] =
          clamp(luma_grain_block[i * luma_grain_stride + j] +
                    ((wsum + rounding_offset) >> params.ar_coeff_shift),
                grain_min_, grain_max_);
    }
  }
}

// Fills cb_grain_block and cr_grain_block with scaled Gaussian noise, then
// applies the AR filter including optional luma coupling (pred_pos[pos][2]==1).
void FilmGrainSynthesizer::generate_chroma_grain_blocks(
    const FilmGrainParams& params,
    const std::vector<std::array<int, 3>>& pred_pos_chroma,
    const int* luma_grain_block,
    int* cb_grain_block, int* cr_grain_block,
    int luma_grain_stride,
    int chroma_block_size_y, int chroma_block_size_x, int chroma_grain_stride,
    int left_pad, int top_pad, int right_pad, int bottom_pad,
    int chroma_subsamp_y, int chroma_subsamp_x) {
  const int bit_depth = params.bit_depth;
  const int gauss_sec_shift = 12 - bit_depth + params.grain_scale_shift;
  const int num_pos = static_cast<int>(pred_pos_chroma.size());
  const int rounding_offset = (1 << (params.ar_coeff_shift - 1));
  const int chroma_grain_block_size = chroma_block_size_y * chroma_grain_stride;

  // Cb — separate PRNG seed 7<<5 per spec Section 7.18.3.2
  if (params.num_cb_points > 0 || params.chroma_scaling_from_luma) {
    init_random_generator(7 << 5, params.random_seed);
    for (int i = 0; i < chroma_block_size_y; i++)
      for (int j = 0; j < chroma_block_size_x; j++)
        cb_grain_block[i * chroma_grain_stride + j] =
            (gaussian_sequence[get_random_number(gauss_bits)] +
             ((1 << gauss_sec_shift) >> 1)) >>
            gauss_sec_shift;
  } else {
    std::fill(cb_grain_block, cb_grain_block + chroma_grain_block_size, 0);
  }

  // Cr — separate PRNG seed 11<<5
  if (params.num_cr_points > 0 || params.chroma_scaling_from_luma) {
    init_random_generator(11 << 5, params.random_seed);
    for (int i = 0; i < chroma_block_size_y; i++)
      for (int j = 0; j < chroma_block_size_x; j++)
        cr_grain_block[i * chroma_grain_stride + j] =
            (gaussian_sequence[get_random_number(gauss_bits)] +
             ((1 << gauss_sec_shift) >> 1)) >>
            gauss_sec_shift;
  } else {
    std::fill(cr_grain_block, cr_grain_block + chroma_grain_block_size, 0);
  }

  // AR filter for both chroma planes simultaneously
  for (int i = top_pad; i < chroma_block_size_y - bottom_pad; i++) {
    for (int j = left_pad; j < chroma_block_size_x - right_pad; j++) {
      int wsum_cb = 0;
      int wsum_cr = 0;
      for (int pos = 0; pos < num_pos; pos++) {
        if (pred_pos_chroma[pos][2] == 0) {
          // Autoregressive term from chroma neighbours
          wsum_cb += params.ar_coeffs_cb[pos] *
                     cb_grain_block[(i + pred_pos_chroma[pos][0]) * chroma_grain_stride +
                                     j + pred_pos_chroma[pos][1]];
          wsum_cr += params.ar_coeffs_cr[pos] *
                     cr_grain_block[(i + pred_pos_chroma[pos][0]) * chroma_grain_stride +
                                     j + pred_pos_chroma[pos][1]];
        } else {
          // Luma coupling: average the corresponding luma grain samples.
          // For 4:2:0 each chroma sample maps to a 2x2 luma region.
          int luma_coord_y = ((i - top_pad) << chroma_subsamp_y) + top_pad;
          int luma_coord_x = ((j - left_pad) << chroma_subsamp_x) + left_pad;
          int av_luma = 0;
          for (int k = luma_coord_y; k < luma_coord_y + chroma_subsamp_y + 1; k++)
            for (int l = luma_coord_x; l < luma_coord_x + chroma_subsamp_x + 1; l++)
              av_luma += luma_grain_block[k * luma_grain_stride + l];
          av_luma = (av_luma +
                     ((1 << (chroma_subsamp_y + chroma_subsamp_x)) >> 1)) >>
                    (chroma_subsamp_y + chroma_subsamp_x);
          wsum_cb += params.ar_coeffs_cb[pos] * av_luma;
          wsum_cr += params.ar_coeffs_cr[pos] * av_luma;
        }
      }
      if (params.num_cb_points > 0 || params.chroma_scaling_from_luma)
        cb_grain_block[i * chroma_grain_stride + j] =
            clamp(cb_grain_block[i * chroma_grain_stride + j] +
                      ((wsum_cb + rounding_offset) >> params.ar_coeff_shift),
                  grain_min_, grain_max_);
      if (params.num_cr_points > 0 || params.chroma_scaling_from_luma)
        cr_grain_block[i * chroma_grain_stride + j] =
            clamp(cr_grain_block[i * chroma_grain_stride + j] +
                      ((wsum_cr + rounding_offset) >> params.ar_coeff_shift),
                  grain_min_, grain_max_);
    }
  }
}

// ---------------------------------------------------------------------------
// synthesizeFgSeg — main entry point
//
// Computes scaling LUTs and grain templates from parsed film grain params.
// Pixel application (tiling, overlap blending, noise addition) is handled
// by the HW decoder using the FilmGrainParams struct directly.
// ---------------------------------------------------------------------------
int FilmGrainSynthesizer::synthesizeFgSeg(
    const FilmGrainParams& params,
    int chroma_subsamp_x, int chroma_subsamp_y,
    FgSynthesisOutput& out) {

  if (!params.apply_grain)
    return -1;

  // Grain sample range for this bit depth (8-bit: [-128, 127])
  const int bit_depth    = static_cast<int>(params.bit_depth);
  const int grain_center = 128 << (bit_depth - 8);
  grain_min_ = -grain_center;
  grain_max_ =  grain_center - 1;

  // Padding constants — same as AOM, always sized for max lag=3
  static const int left_pad   = 3;
  static const int right_pad  = 3;
  static const int top_pad    = 3;
  static const int bottom_pad = 0;
  static const int ar_padding = 3;

  const int chroma_subblock_size_y = luma_subblock_size_y >> chroma_subsamp_y;
  const int chroma_subblock_size_x = luma_subblock_size_x >> chroma_subsamp_x;

  const int luma_block_size_y =
      top_pad + 2 * ar_padding + luma_subblock_size_y * 2 + bottom_pad;
  const int luma_block_size_x =
      left_pad + 2 * ar_padding + luma_subblock_size_x * 2 + 2 * ar_padding + right_pad;

  const int chroma_block_size_y =
      top_pad + (2 >> chroma_subsamp_y) * ar_padding +
      chroma_subblock_size_y * 2 + bottom_pad;
  const int chroma_block_size_x =
      left_pad + (2 >> chroma_subsamp_x) * ar_padding +
      chroma_subblock_size_x * 2 + (2 >> chroma_subsamp_x) * ar_padding + right_pad;

  const int luma_grain_stride   = luma_block_size_x;
  const int chroma_grain_stride = chroma_block_size_x;

  // AR prediction position tables
  const int num_pos_luma   = 2 * params.ar_coeff_lag * (params.ar_coeff_lag + 1);
  const int num_pos_chroma = num_pos_luma + (params.num_y_points > 0 ? 1 : 0);

  std::vector<std::array<int, 3>> pred_pos_luma(num_pos_luma);
  std::vector<std::array<int, 3>> pred_pos_chroma(num_pos_chroma);

  {
    int pos = 0;
    for (int row = -params.ar_coeff_lag; row < 0; row++)
      for (int col = -params.ar_coeff_lag; col <= params.ar_coeff_lag; col++) {
        pred_pos_luma[pos]   = {row, col, 0};
        pred_pos_chroma[pos] = {row, col, 0};
        ++pos;
      }
    for (int col = -params.ar_coeff_lag; col < 0; col++) {
      pred_pos_luma[pos]   = {0, col, 0};
      pred_pos_chroma[pos] = {0, col, 0};
      ++pos;
    }
    if (params.num_y_points > 0)
      pred_pos_chroma[pos] = {0, 0, 1};
  }

  // Allocate and generate grain templates
  out.luma_grain.assign(luma_block_size_y   * luma_grain_stride,   0);
  out.cb_grain.assign  (chroma_block_size_y * chroma_grain_stride, 0);
  out.cr_grain.assign  (chroma_block_size_y * chroma_grain_stride, 0);

  generate_luma_grain_block(params, pred_pos_luma, out.luma_grain.data(),
                            luma_block_size_y, luma_block_size_x, luma_grain_stride,
                            left_pad, top_pad, right_pad, bottom_pad);

  generate_chroma_grain_blocks(params, pred_pos_chroma, out.luma_grain.data(),
                               out.cb_grain.data(), out.cr_grain.data(),
                               luma_grain_stride,
                               chroma_block_size_y, chroma_block_size_x, chroma_grain_stride,
                               left_pad, top_pad, right_pad, bottom_pad,
                               chroma_subsamp_y, chroma_subsamp_x);

  // Scaling LUTs
  init_scaling_function(params.scaling_points_y.data(),  params.num_y_points,  out.scaling_lut_y);
  if (params.chroma_scaling_from_luma) {
    out.scaling_lut_cb = out.scaling_lut_y;
    out.scaling_lut_cr = out.scaling_lut_y;
  } else {
    init_scaling_function(params.scaling_points_cb.data(), params.num_cb_points, out.scaling_lut_cb);
    init_scaling_function(params.scaling_points_cr.data(), params.num_cr_points, out.scaling_lut_cr);
  }

  // Store template dimensions for the caller
  out.luma_block_size_y   = luma_block_size_y;
  out.luma_block_size_x   = luma_block_size_x;
  out.chroma_block_size_y = chroma_block_size_y;
  out.chroma_block_size_x = chroma_block_size_x;

  AV1_FG_DBG("synthesizeFgSeg: seed=%u lag=%d luma=%dx%d chroma=%dx%d",
             params.random_seed, params.ar_coeff_lag,
             luma_block_size_y, luma_block_size_x,
             chroma_block_size_y, chroma_block_size_x);

  return 0;
}

// ---------------------------------------------------------------------------
// Debug dump — matches dump_film_grain() in aom/.../examples/av1dec_diag.c
// Output is identical so you can diff your log against AOM diagnostic output.
// ---------------------------------------------------------------------------
void fg_dbg_dump_params(const FilmGrainParams& fg, FILE* out) {
  fprintf(out, "[FILM_GRAIN]\n");
  fprintf(out, "  apply_grain           : %d\n", fg.apply_grain ? 1 : 0);
  if (!fg.apply_grain) return;

  fprintf(out, "  update_parameters     : %d\n", fg.update_parameters ? 1 : 0);
  fprintf(out, "  random_seed           : %u\n", static_cast<unsigned int>(fg.random_seed));
  fprintf(out, "  bit_depth             : %u\n", fg.bit_depth);
  fprintf(out, "  scaling_shift         : %d\n", fg.scaling_shift);
  fprintf(out, "  ar_coeff_lag          : %d\n", fg.ar_coeff_lag);
  fprintf(out, "  ar_coeff_shift        : %d\n", fg.ar_coeff_shift);
  fprintf(out, "  grain_scale_shift     : %d\n", fg.grain_scale_shift);
  fprintf(out, "  overlap_flag          : %d\n", fg.overlap_flag ? 1 : 0);
  fprintf(out, "  clip_to_restricted    : %d\n", fg.clip_to_restricted_range ? 1 : 0);
  fprintf(out, "  chroma_from_luma      : %d\n", fg.chroma_scaling_from_luma ? 1 : 0);
  fprintf(out, "  cb_mult/luma_mult/off : %d / %d / %d\n",
          fg.cb_mult, fg.cb_luma_mult, fg.cb_offset);
  fprintf(out, "  cr_mult/luma_mult/off : %d / %d / %d\n",
          fg.cr_mult, fg.cr_luma_mult, fg.cr_offset);

  fprintf(out, "  num_y_points          : %d\n", fg.num_y_points);
  for (int i = 0; i < fg.num_y_points; i++)
    fprintf(out, "    y_point[%d] = (%d, %d)\n",
            i, fg.scaling_points_y[i][0], fg.scaling_points_y[i][1]);

  fprintf(out, "  num_cb_points         : %d\n", fg.num_cb_points);
  for (int i = 0; i < fg.num_cb_points; i++)
    fprintf(out, "    cb_point[%d] = (%d, %d)\n",
            i, fg.scaling_points_cb[i][0], fg.scaling_points_cb[i][1]);

  fprintf(out, "  num_cr_points         : %d\n", fg.num_cr_points);
  for (int i = 0; i < fg.num_cr_points; i++)
    fprintf(out, "    cr_point[%d] = (%d, %d)\n",
            i, fg.scaling_points_cr[i][0], fg.scaling_points_cr[i][1]);

  // AR coefficients — only print when lag > 0 (num_pos == 0 when lag == 0)
  const int num_pos = 2 * fg.ar_coeff_lag * (fg.ar_coeff_lag + 1);
  if (num_pos > 0) {
    fprintf(out, "  ar_coeffs_y[0..%d]    :", num_pos - 1);
    for (int i = 0; i < num_pos; i++)
      fprintf(out, " %d", fg.ar_coeffs_y[i]);
    fprintf(out, "\n");

    // Chroma has one extra coefficient (the luma-coupling term at index num_pos)
    fprintf(out, "  ar_coeffs_cb[0..%d]   :", num_pos);
    for (int i = 0; i <= num_pos; i++)
      fprintf(out, " %d", fg.ar_coeffs_cb[i]);
    fprintf(out, "\n");

    fprintf(out, "  ar_coeffs_cr[0..%d]   :", num_pos);
    for (int i = 0; i <= num_pos; i++)
      fprintf(out, " %d", fg.ar_coeffs_cr[i]);
    fprintf(out, "\n");
  }
}

// Prints a rectangular region of a grain template block for visual inspection
// or diffing against AOM.  Values are printed as a 2-D grid, 16 per row.
void fg_dbg_dump_grain_block(const char* label,
                             const int* block, int grain_stride,
                             int offset_y, int offset_x,
                             int rows, int cols,
                             FILE* out) {
  fprintf(out, "[GRAIN_BLOCK %s  offset=(%d,%d)  size=%dx%d]\n",
          label, offset_y, offset_x, rows, cols);
  for (int i = 0; i < rows; i++) {
    const int* row_ptr = block + (offset_y + i) * grain_stride + offset_x;
    for (int j = 0; j < cols; j++) {
      fprintf(out, "%5d", row_ptr[j]);
      if ((j & 15) == 15 || j == cols - 1)
        fprintf(out, "\n");
    }
  }
}

} // namespace av1
