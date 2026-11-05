#include "ysfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void near(double actual, double expected, double tolerance, const char *message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s: %.8f (expected %.8f ± %.8f)\n",
                     message, actual, expected, tolerance);
        std::exit(1);
    }
}

struct Effect {
    ysfx_config_t *config = ysfx_config_new();
    ysfx_t *fx = ysfx_new(config);
    double rate;
    double phase = 0;
    double last_input = 0;
    double last_output = 0;

    explicit Effect(double sample_rate = 48000) : rate(sample_rate)
    {
        check(ysfx_load_file(fx, "jsfx/AZMhybridAGC.jsfx", 0), "load JSFX");
        check(ysfx_compile(fx, 0), "compile JSFX and GUI");
        ysfx_set_sample_rate(fx, rate);
        ysfx_set_block_size(fx, 512);
        ysfx_time_info_t time{};
        time.playback_state = ysfx_playback_paused;
        ysfx_set_time_info(fx, &time);
        ysfx_init(fx);
    }
    ~Effect() { ysfx_free(fx); ysfx_config_free(config); }
    void set(int index, double value) { ysfx_slider_set_value(fx, index - 1, value); }
    double var(const char *name)
    {
        auto *p = ysfx_find_var(fx, name);
        check(p != nullptr, name);
        return *p;
    }
    void set_var(const char *name, double value)
    {
        auto *p = ysfx_find_var(fx, name);
        check(p != nullptr, name);
        *p = value;
    }
    void run(double seconds, double rms_db, bool silence = false)
    {
        int remaining = static_cast<int>(std::llround(seconds * rate));
        const double amplitude = std::sqrt(2.0) * std::pow(10.0, rms_db / 20);
        while (remaining > 0) {
            int n = std::min(remaining, 512);
            std::vector<double> l(n), r(n), out_l(n), out_r(n);
            for (int i = 0; i < n; ++i) {
                l[i] = silence ? 0 : amplitude * std::sin(phase);
                r[i] = l[i] * 0.5;
                phase += 2 * std::acos(-1.0) * 1000 / rate;
            }
            const double *input[] = {l.data(), r.data()};
            double *output[] = {out_l.data(), out_r.data()};
            ysfx_process_double(fx, input, output, 2, 2, n);
            for (int i = 0; i < n; ++i) {
                check(std::isfinite(out_l[i]) && std::isfinite(out_r[i]), "finite output");
                near(out_r[i], out_l[i] * 0.5, 1e-10, "stereo-linked gain");
            }
            last_input = l.back();
            last_output = out_l.back();
            remaining -= n;
        }
    }
};

static void test_measurement_and_mix(double rate)
{
    Effect e(rate);
    e.set(7, 0);
    e.set(5, 0);
    e.set(6, 50);
    e.run(0.39, -20);
    check(e.var("have_measurement") == 0, "no complete 400 ms block yet");
    e.run(0.61, -20);
    check(e.var("have_measurement") == 1, "measures without playback");
    // Left RMS is -20 dBFS, right is 6.02 dB lower; RMS averages the two powers.
    const double rms = -20 + 10 * std::log10((1 + 0.25) / 2);
    near(e.var("rms_db"), rms, 0.08, "combined RMS");
    // Independent BS.1770 filter frequency response for a 1 kHz stereo sine.
    near(e.var("loudness_db"), rate < 48000 ? -19.02144 :
         rate == 48000 ? -19.02420 : -19.04150, 0.04, "K-weighted loudness");
    near(e.var("hybrid_db"), (e.var("loudness_db") + rms) / 2, 0.08, "50 percent hybrid");
    e.set(6, 100);
    e.set(5, 2);
    e.run(0.2, -20);
    near(e.var("hybrid_db"), rms + 2, 0.08, "RMS-only hybrid and calibration");
    e.set(6, 0);
    e.run(0.2, -20);
    near(e.var("hybrid_db"), e.var("loudness_db"), 0.001, "loudness-only hybrid");
}

static void test_smoothing_and_limits()
{
    Effect e;
    e.set(7, 0);
    e.set(6, 100);
    e.set(5, 0);
    e.set(3, 6);
    e.set(11, 0);
    e.set(1, -16);
    e.run(0.4, -20);
    near(e.var("gain_db"), 0, 0.01, "no jump at first measurement");
    e.run(0.2, -20);
    near(e.var("gain_db"), 3, 0.05, "gain follows the 0.4 second smoothing window");
    e.run(1.0, -20);
    near(e.var("gain_db"), 6, 0.05, "gain converges to the boost limit");
    e.set(3, 2);
    e.run(4.0, -20);
    near(e.var("gain_db"), 2, 0.1, "boost limit");
    Effect loud;
    loud.set(7, 0);
    loud.set(6, 100);
    loud.set(5, 0);
    loud.set(2, 2);
    loud.run(4, -5);
    near(loud.var("gain_db"), -2, 0.03, "attenuation limit");
    Effect very_quiet;
    very_quiet.set(7, 0);
    very_quiet.run(1, -90);
    check(very_quiet.var("have_measurement") == 0, "absolute gate rejects very quiet input");
    near(very_quiet.var("gain_db"), 0, 0.001, "no boost before gated measurement");
}

static void test_defaults()
{
    Effect e;
    near(ysfx_slider_get_value(e.fx, 1), 6, 0, "default Push Down");
    near(ysfx_slider_get_value(e.fx, 2), 3, 0, "default Pull Up");
    near(ysfx_slider_get_value(e.fx, 4), 3, 0, "default Calibration");
    near(ysfx_slider_get_value(e.fx, 5), 40, 0, "default Ratio");
    near(ysfx_slider_get_value(e.fx, 10), 25, 0, "default Short Mix");
}

// A larger limit moves the gain faster, but a limit of 0 must still let the gain return.
static void test_limit_speed()
{
    auto gain_after = [](double push_down, double pull_up, double seconds, double rms_db) {
        Effect e;
        e.set(7, 0);
        e.set(6, 100);
        e.set(5, 3.5);
        e.set(11, 0);
        e.set(2, push_down);
        e.set(3, pull_up);
        e.run(seconds, rms_db);
        return e.var("gain_db");
    };
    // The target boost is about 2.5 dB, below both limits.
    const double fast_up = gain_after(6, 6, 0.9, -20);
    const double slow_up = gain_after(6, 3, 0.9, -20);
    check(slow_up > 0.1 && slow_up < fast_up - 0.1, "Pull Up scales the speed of boosting");
    near(gain_after(6, 3, 12, -20), 2.5, 0.1, "slow boost still reaches the target");
    // The target cut is about 2.5 dB, below both limits.
    const double fast_down = gain_after(6, 6, 0.7, -14);
    const double slow_down = gain_after(3.6, 6, 0.7, -14);
    check(slow_down < -0.1 && slow_down > fast_down + 0.1, "Push Down scales the speed of cutting");

    Effect e;
    e.set(7, 0);
    e.set(6, 100);
    e.set(5, 0);
    e.set(11, 0);
    e.run(6, -5);
    check(e.var("gain_db") < -5.5, "test has applied cut");
    e.set(2, 0);
    e.run(15, -5);
    near(e.var("gain_db"), 0, 0.1, "gain returns to 0 dB even with Push Down at 0");
}

static void test_short_mix()
{
    auto gain_after = [](double short_mix, double loud_db, double loud_seconds) {
        Effect e;
        e.set(7, 0);
        e.set(6, 100);
        e.set(5, 0);
        e.set(11, short_mix);
        e.run(20, -24);
        e.run(loud_seconds, loud_db);
        return e.var("gain_db");
    };
    // Heavy section after a long quiet one: the short window pulls the gain down sooner.
    const double slow_down = gain_after(0, -12, 2);
    const double fast_down = gain_after(100, -12, 2);
    check(fast_down < slow_down - 1, "Short Mix reacts to a sudden louder section");
    // Quiet section after a long loud one: the relative gate drops it from the cumulative level,
    // so only the short window brings the gain back.
    Effect cumulative;
    Effect shortterm;
    for (Effect *e : {&cumulative, &shortterm}) {
        e->set(7, 0);
        e->set(6, 100);
        e->set(5, 0);
        e->set(3, 6);
        e->run(20, -10);
    }
    cumulative.set(11, 0);
    shortterm.set(11, 100);
    cumulative.run(4, -30);
    shortterm.run(4, -30);
    check(shortterm.var("gain_db") > cumulative.var("gain_db") + 1,
          "Short Mix lets the gain recover after a quiet section");
}

// A louder short level cuts the gain at once; a quieter one lifts it only after a release.
static void test_short_asymmetry()
{
    Effect loud;
    Effect quiet;
    for (Effect *e : {&loud, &quiet}) {
        e->set(7, 0);
        e->set(6, 100);
        e->set(5, 0);
        e->set(3, 6);
        e->set(11, 100);
    }
    loud.run(20, -24);
    loud.run(0.6, -12);
    check(loud.var("gain_db") < 0, "loud passage cuts the gain within 0.6 seconds");
    quiet.run(20, -10);
    quiet.run(0.5, -30);
    check(quiet.var("gain_db") < -3, "quiet passage does not lift the gain at once");
    quiet.run(3.5, -30);
    check(quiet.var("gain_db") > 5.5, "quiet passage lifts the gain after the release");
}

static void test_gate_and_silence()
{
    Effect e;
    e.set(7, 0);
    e.run(4.0, -20);
    const double initial = e.var("rms_db");
    e.run(4.0, -35);
    near(e.var("rms_db"), initial, 0.2, "relative gate rejects quiet blocks");
    e.run(90.0, -35);
    check(e.var("rms_db") < -28, "previously gated blocks are reconsidered");
    e.run(6.0, -100, true);
    check(e.var("have_measurement") == 1, "silence reset can be disabled");
    e.set(7, 1);
    e.run(5.5, -100, true);
    check(e.var("have_measurement") == 0, "silence reset clears statistics");
    e.run(1.0, -16);
    check(e.var("have_measurement") == 1, "measurement resumes after silence");
}

static void test_playback_reset()
{
    Effect e;
    e.set(7, 0);
    e.set(8, 1);
    e.run(1, -20);
    ysfx_time_info_t time{};
    time.playback_state = ysfx_playback_playing;
    ysfx_set_time_info(e.fx, &time);
    e.run(0.1, -20);
    check(e.var("have_measurement") == 0, "playback transition resets statistics");
    e.run(0.5, -20);
    check(e.var("have_measurement") == 1, "playback continues measuring");
    time.playback_state = ysfx_playback_paused;
    ysfx_set_time_info(e.fx, &time);
    e.run(1, -20);
    check(e.var("have_measurement") == 1, "paused transport still processes input");
    e.set(8, 0);
    time.playback_state = ysfx_playback_playing;
    ysfx_set_time_info(e.fx, &time);
    e.run(0.1, -20);
    check(e.var("have_measurement") == 1, "playback reset can be disabled");
}

static void test_silence_timing_and_rate_change()
{
    Effect e;
    e.run(1, -20);
    e.run(4.9, -100, true);
    check(e.var("have_measurement") == 1, "silence does not reset before five seconds");
    e.run(0.1, -100, true);
    check(e.var("have_measurement") == 0, "silence resets at five seconds");
    e.run(1, -20);
    check(e.var("have_measurement") == 1, "audio resumes after silence reset");
    ysfx_set_sample_rate(e.fx, 44100);
    e.rate = 44100;
    e.run(0.1, -20);
    check(e.var("have_measurement") == 0, "sample-rate change resets metering");
    e.run(0.4, -20);
    check(e.var("have_measurement") == 1, "metering resumes at new sample rate");
}

static void test_gui()
{
    Effect e;
    near(ysfx_slider_get_value(e.fx, 9), 0, 0, "settings are closed by default");
    e.run(1, -20);
    std::vector<uint8_t> pixels(570 * 245 * 4);
    ysfx_gfx_config_t config{};
    config.pixel_width = 570;
    config.pixel_height = 245;
    config.scale_factor = 1;
    config.pixels = pixels.data();
    ysfx_gfx_setup(e.fx, &config);
    auto frame = [&](int x, int y, bool down) {
        ysfx_gfx_update_mouse(e.fx, 0, x, y, down ? ysfx_button_left : 0, 0, 0);
        ysfx_gfx_run(e.fx);
    };
    auto click = [&](int x, int y) { frame(x, y, true); frame(x, y, false); };
    auto save = [&](const char *name) {
        const char *dir = std::getenv("AZM_TEST_RENDER_DIR");
        if (!dir) return;
        std::string path = std::string(dir) + "/" + name + ".ppm";
        FILE *file = std::fopen(path.c_str(), "wb");
        check(file != nullptr, "open GUI render");
        std::fprintf(file, "P6\n570 245\n255\n");
        for (size_t i = 0; i < pixels.size(); i += 4) {
            std::fputc(pixels[i + 2], file);
            std::fputc(pixels[i + 1], file);
            std::fputc(pixels[i], file);
        }
        check(std::fclose(file) == 0, "write GUI render");
    };
    frame(0, 0, false);
    save("closed");
    const double closed_meter_h = e.var("pl_h");
    const double closed_meter_y = e.var("pl_y");
    check(closed_meter_h > 0, "meter has a height");
    // The pointer coordinates below are in design units (570 x 245 window, scale 1).
    click(470, 31);
    near(ysfx_slider_get_value(e.fx, 8), 1, 0, "bypass button");
    click(470, 31);
    click(100, 73);
    near(ysfx_slider_get_value(e.fx, 6), 0, 0, "silence button");
    click(100, 105);
    near(ysfx_slider_get_value(e.fx, 7), 1, 0, "playback button");
    click(100, 137);
    e.run(0.01, -20);
    check(e.var("have_measurement") == 0, "reset button");
    click(362, 31);
    near(ysfx_slider_get_value(e.fx, 9), 1, 0, "open settings");
    frame(0, 0, false);
    save("open");
    near(e.var("pl_h"), closed_meter_h, 0, "meter height is the same with settings open");
    near(e.var("pl_y"), closed_meter_y, 0, "meter position is the same with settings open");
    // Each dial must respond independently to a vertical drag.
    const int knob_sliders[] = {0, 1, 2, 10, 4, 5};
    for (int i = 0; i < 6; ++i) {
        const int x = int(60 + 434.0 / 6 * i + (i >= 4 ? 16 : 0) + 38);
        const double before = ysfx_slider_get_value(e.fx, knob_sliders[i]);
        frame(x, 100, true);
        frame(x, 115, true);
        frame(x, 115, false);
        check(ysfx_slider_get_value(e.fx, knob_sliders[i]) < before, "knob drag changes its slider");
    }
    click(362, 31);
    near(ysfx_slider_get_value(e.fx, 9), 0, 0, "close settings");
    frame(0, 0, false);
    near(e.var("pl_h"), closed_meter_h, 0, "meter height is the same after closing settings");
}

static void test_manual_reset_and_bypass()
{
    Effect e;
    e.set(7, 0);
    e.set(6, 100);
    e.run(5.0, -25);
    check(e.var("gain_db") > 1, "test has applied gain");
    e.set(9, 1);
    e.run(0.1, -25);
    near(e.last_output, e.last_input, 1e-6, "bypass fades to dry");
    check(e.var("have_measurement") == 1, "bypass keeps measuring");
    e.set_var("reset_request", 1);
    e.run(0.1, -25);
    check(e.var("have_measurement") == 0, "manual reset clears measurement");
}

int main()
{
    test_measurement_and_mix(44100);
    test_measurement_and_mix(44101);
    test_measurement_and_mix(48000);
    test_measurement_and_mix(96000);
    test_defaults();
    test_smoothing_and_limits();
    test_limit_speed();
    test_short_mix();
    test_short_asymmetry();
    test_gate_and_silence();
    test_playback_reset();
    test_silence_timing_and_rate_change();
    test_manual_reset_and_bypass();
    test_gui();
    std::puts("JSFX integration tests passed");
}
