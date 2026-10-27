/*
 * fit_defaults.cpp — where should the pots sit by default?
 *
 * "Default" on a 606 clone means one thing: the pot positions that make each
 * voice sound like the hardware with the knobs at rest. This searches each
 * voice's pots against the matching hardware recording, through the REAL
 * engine, and reports the best pot set with its score against the current
 * defaults. Drive, distortion and level are left at their defaults — they are
 * not part of what a 606 sounds like.
 *
 *   ./fit_defaults <dir with the Clean Kit wavs> [voice-id]
 *
 * Results go into scripts/gen_params.py by hand, with the numbers quoted.
 */
#include "score.h"
#include "sd606_engine.h"

struct Axis { const char *key; std::vector<int> vals; };
struct Voice { const char *file; int voice; const char *id; std::vector<Axis> axes; };

static std::vector<int> range(int lo, int hi, int step) { std::vector<int> v; for(int i = lo; i <= hi; i += step) v.push_back(i); return v; }

static double eval(const Voice &vc, const std::vector<int> &pots, const std::vector<float> &ref,
                   const Profile &pr, int rate, Score *out)
{
    sd606_engine_t *e = sd606_create((float)rate);
    sd606_set_param(e, "volume", "127"); sd606_set_param(e, "master_dist", "0");
    char val[16];
    for(size_t i = 0; i < pots.size(); ++i) { snprintf(val, sizeof val, "%d", pots[i]); sd606_set_param(e, vc.axes[i].key, val); }
    sd606_trigger(e, vc.voice, 90);
    std::vector<float> got(ref.size() + 4096); float blk[128];
    for(size_t i = 0; i < got.size(); i += 128) { sd606_render(e, blk, 128); for(int k = 0; k < 128 && i+k < got.size(); ++k) got[i+k] = blk[k]; }
    sd606_destroy(e);
    trim_onset(got); got.resize(ref.size(), 0.0f);
    Profile pg; profile(got, rate, pg, ref.size());
    const Score s = score(pr, pg);
    if(out) *out = s;
    return s.total();
}

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: fit_defaults <dir> [voice]\n"); return 1; }
    const char *only = argc > 2 ? argv[2] : NULL;
    const std::vector<Voice> voices = {
        { "BD 606 Clean.wav",     SD606_BD, "bd", { {"bd_tune", range(16,76,4)}, {"bd_decay", range(0,64,4)}, {"bd_attack", range(48,127,8)} } },
        { "Snare 606 Clean.wav",  SD606_SD, "sd", { {"sd_tune", range(46,82,6)}, {"sd_decay", range(40,127,12)}, {"sd_snappy", range(32,127,16)}, {"sd_tone", range(40,88,12)} } },
        { "Tom Lo Clean 606.wav", SD606_LT, "lt", { {"lt_tune", range(48,84,2)}, {"lt_decay", range(60,127,4)} } },
        { "Tom Hi Clean 606.wav", SD606_HT, "ht", { {"ht_tune", range(48,84,2)}, {"ht_decay", range(60,127,4)} } },
        { "CH 606 Clean.wav",     SD606_CH, "ch", { {"ch_tune", range(48,96,4)}, {"ch_decay", range(60,127,4)} } },
        { "OH 606 Clean.wav",     SD606_OH, "oh", { {"oh_tune", range(48,96,4)}, {"oh_decay", range(40,100,4)} } },
        { "Cymbal 606 Clean.wav", SD606_CY, "cy", { {"cy_tune", range(52,76,4)}, {"cy_decay", range(40,127,8)} } },
    };
    for(const Voice &vc : voices)
    {
        if(only && strcmp(only, vc.id)) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], vc.file);
        int rate; std::vector<float> ref = load_wav(path, &rate); trim_onset(ref);
        Profile pr; profile(ref, rate, pr, ref.size());

        /* current defaults: read them back from a fresh engine */
        std::vector<int> cur(vc.axes.size());
        { sd606_engine_t *e = sd606_create((float)rate); char b[16];
          for(size_t i = 0; i < vc.axes.size(); ++i) { sd606_get_param(e, vc.axes[i].key, b, sizeof b); cur[i] = atoi(b); }
          sd606_destroy(e); }
        Score cs; const double cur_score = eval(vc, cur, ref, pr, rate, &cs);

        /* exhaustive grid */
        std::vector<size_t> idx(vc.axes.size(), 0);
        std::vector<int> best = cur; Score bs = cs; double best_score = cur_score; long tried = 0;
        for(;;)
        {
            std::vector<int> pots(vc.axes.size());
            for(size_t i = 0; i < idx.size(); ++i) pots[i] = vc.axes[i].vals[idx[i]];
            Score s; const double sc = eval(vc, pots, ref, pr, rate, &s); ++tried;
            if(sc < best_score) { best_score = sc; best = pots; bs = s; }
            size_t k = 0;
            while(k < idx.size() && ++idx[k] == vc.axes[k].vals.size()) { idx[k] = 0; ++k; }
            if(k == idx.size()) break;
        }
        printf("=== %s  (%ld renders) ===\n", vc.id, tried);
        printf("  current :"); for(size_t i = 0; i < cur.size(); ++i) printf(" %s=%d", vc.axes[i].key, cur[i]);
        printf("   -> spectral %.2f env %.2f crest %.2f  score %.2f\n", cs.spectral, cs.envelope, cs.crest, cur_score);
        printf("  best    :"); for(size_t i = 0; i < best.size(); ++i) printf(" %s=%d", vc.axes[i].key, best[i]);
        printf("   -> spectral %.2f env %.2f crest %.2f  score %.2f\n", bs.spectral, bs.envelope, bs.crest, best_score);
    }
    return 0;
}
