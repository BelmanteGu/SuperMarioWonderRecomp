package io.github.belmantegu.wonderrecomp;

import java.util.ArrayList;
import java.util.List;

/**
 * The option rows, in the order of the mockup. Every row that changes the game has a reader in the runtime (the
 * "consumer" comment says where; docs/WONDER-ANDROID-OPTIONS.md has the table). Rows of the mockup the runtime does not
 * read are not here: fxaa, fps_cap, widescreen, display_res, haptics, floating stick, volume, audio latency, the
 * driver tab and the report sender.
 */
final class Props {
    static final String T_IMAGE = "image", T_PERF = "perf", T_CONTROLS = "controls", T_AUDIO = "audio", T_DATA = "data",
            T_DIAG = "diag", T_ABOUT = "about";
    static final String[] TABS = {T_IMAGE, T_PERF, T_CONTROLS, T_AUDIO, T_DATA, T_DIAG, T_ABOUT};
    static final String[] TAB_ICONS = {"image", "gauge", "gamepad-2", "volume-2", "folder", "activity", "info"};

    enum Kind { CHOICE, TOGGLE, SLIDER, ACTION, INFO }

    /** CONFIG: config.txt (the runtime reads it), APP: app.txt (the launcher), NONE: no stored value. */
    enum Store { CONFIG, APP, NONE }

    static final class Prop {
        final String id, tab, key, dflt, consumer;
        final Kind kind;
        final Store store;
        /** Stored text of each choice; null = no line (the runtime's own default). Toggles: [off, on]. */
        final String[] values;
        final String[] en, pt;
        boolean restart, advanced, experimental;
        /** Always written as a line (the runtime's default differs from the row's meaning). */
        boolean explicit;
        String requires;
        int min, max, step, sliderDefault;

        Prop(String id, String tab, Kind kind, Store store, String key, String dflt, String[] values, String[] en,
             String[] pt, String consumer) {
            this.id = id;
            this.tab = tab;
            this.kind = kind;
            this.store = store;
            this.key = key;
            this.dflt = dflt;
            this.values = values;
            this.en = en;
            this.pt = pt;
            this.consumer = consumer;
        }

        Prop restart() {
            restart = true;
            return this;
        }

        Prop advanced() {
            advanced = true;
            return this;
        }

        Prop experimental() {
            experimental = true;
            return this;
        }

        Prop explicit() {
            explicit = true;
            return this;
        }

        Prop requires(String id) {
            requires = id;
            return this;
        }

        int indexOf(String v) {
            for (int i = 0; i < values.length; i++) {
                if (values[i] == null ? v == null : values[i].equals(v)) return i;
            }
            return -1;
        }
    }

    private static final String[] OFF_ON = {"0", "1"};
    private static final String[] OFF_ON_EN = {"Off", "On"};
    private static final String[] OFF_ON_PT = {"Desligado", "Ligado"};

    private static String[] a(String... s) {
        return s;
    }

    private static Prop choice(String id, String tab, Store st, String key, String dflt, String[] values, String[] en, String[] pt, String consumer) {
        return new Prop(id, tab, Kind.CHOICE, st, key, dflt, values, en, pt, consumer);
    }

    private static Prop toggle(String id, String tab, Store st, String key, String dflt, String[] values, String consumer) {
        return new Prop(id, tab, Kind.TOGGLE, st, key, dflt, values, OFF_ON_EN, OFF_ON_PT, consumer);
    }

    private static Prop slider(String id, int min, int max, int step, int dflt, String key, String consumer) {
        Prop p = new Prop(id, T_CONTROLS, Kind.SLIDER, Store.CONFIG, key, null, new String[0], null, null, consumer);
        p.min = min;
        p.max = max;
        p.step = step;
        p.sliderDefault = dflt;
        return p;
    }

    private static Prop action(String id, String tab) {
        return new Prop(id, tab, Kind.ACTION, Store.NONE, null, null, new String[0], null, null, null);
    }

    private static Prop info(String id, String tab) {
        return new Prop(id, tab, Kind.INFO, Store.NONE, null, null, new String[0], null, null, null);
    }

    static List<Prop> all() {
        List<Prop> l = new ArrayList<>();
        // ---- image
        // NX_HANDHELD present = handheld (svc_am.cpp, svc_misc.cpp); platform_android.cpp sets it unless config.txt says 0.
        l.add(choice("handheld", T_IMAGE, Store.CONFIG, "NX_HANDHELD", "1", a("1", "0"),
                a("Handheld (720p)", "Docked / TV (1080p)"), a("Portátil (720p)", "TV (1080p)"),
                "runtime/src/svc_am.cpp getenv(NX_HANDHELD)").restart().explicit());
        l.add(choice("render_height", T_IMAGE, Store.CONFIG, "NX_RENDER_RES", null,
                a(null, "960x540", "1280x720", "1600x900", "1920x1080"),
                a("Console mode's own", "540p (light)", "720p", "900p", "1080p (sharp)"),
                a("O do modo do console", "540p (leve)", "720p", "900p", "1080p (nítido)"),
                "runtime/src/svc_am.cpp getenv(NX_RENDER_RES)").restart());
        l.add(choice("render_scale", T_IMAGE, Store.CONFIG, "render_scale", null, a(null, "0.5", "0.67", "0.75", "0.85", "1"),
                a("GPU default", "0.5x", "0.67x", "0.75x", "0.85x", "1x"),
                a("Padrão da GPU", "0,5x", "0,67x", "0,75x", "0,85x", "1x"),
                "runtime/src/platform_android.cpp setting(render_scale)").restart());
        l.add(toggle("fsr", T_IMAGE, Store.CONFIG, "NX_FSR", "0", OFF_ON, "runtime/src/nvn/nvn_present.cpp getenv(NX_FSR)").restart());
        l.add(choice("fsr_sharpness", T_IMAGE, Store.CONFIG, "NX_FSR_SHARP", null, a(null, "0", "0.5", "1", "1.5", "2"),
                a("Default (0.2)", "Maximum (0)", "High (0.5)", "Medium (1)", "Soft (1.5)", "No sharpening (2)"),
                a("Padrão (0,2)", "Máxima (0)", "Alta (0,5)", "Média (1)", "Suave (1,5)", "Sem nitidez (2)"),
                "runtime/src/nvn/nvn_present.cpp getenv(NX_FSR_SHARP)").restart().requires("fsr"));
        // NX_ULTRAWIDE=1 (study ultrawide-1710): render targets widened to the screen's shape and the cameras' aspect
        // patched, so a 19.5:9 / 20:9 phone shows more of the level instead of side bars. Off = the game's 16:9.
        l.add(choice("ultrawide", T_IMAGE, Store.CONFIG, "NX_ULTRAWIDE", null, a(null, "1"),
                a("Off (16:9, side bars)", "Fill the screen (wider view)"),
                a("Desligado (16:9, faixas nas laterais)", "Preencher a tela (visão mais larga)"),
                "runtime/src/nvn/nvn_ultrawide.cpp getenv(NX_ULTRAWIDE)").restart().experimental());
        // ---- performance
        // NX_FPS_CAP=30 holds the game's own 30 fps mode (nvn_queue.cpp frame_time_probe); no line = the game's own rule.
        l.add(choice("fps_cap", T_PERF, Store.CONFIG, "NX_FPS_CAP", null, a(null, "30"),
                a("60 (game's own: 60, 30 when heavy)", "30 (always, steady)"),
                a("60 (do jogo: 60, 30 quando pesado)", "30 (sempre, estável)"),
                "runtime/src/nvn/nvn_queue.cpp getenv(NX_FPS_CAP)").restart());
        // NX_DISPLAY_HZ (study fps-unlock-1710): the vsync the game paces itself on. The game's logic steps once per
        // frame, so above 60 it also runs faster: an experiment, not a smooth 120.
        l.add(choice("display_hz", T_PERF, Store.CONFIG, "NX_DISPLAY_HZ", null, a(null, "90", "120"),
                a("60 Hz (normal)", "90 Hz (game runs 1.5x fast)", "120 Hz (game runs 2x fast)"),
                a("60 Hz (normal)", "90 Hz (jogo 1,5x mais rápido)", "120 Hz (jogo 2x mais rápido)"),
                "runtime/src/hle_vi.cpp getenv(NX_DISPLAY_HZ)").restart().experimental());
        // No line: pipelined on integrated GPUs (every phone), study intel-igpu-1710; "0" forces synchronous.
        l.add(choice("pipelined", T_PERF, Store.CONFIG, "NX_PIPELINED", null, a(null, "0", "2", "3", "4"),
                a("Automatic (pipelined on phones)", "Synchronous (safe)", "2 in flight", "3 in flight", "4 in flight"),
                a("Automático (em pipeline no celular)", "Síncrono (seguro)", "2 em voo", "3 em voo", "4 em voo"),
                "runtime/src/nvn/nvn_frames.cpp getenv(NX_PIPELINED)").restart().experimental());
        l.add(toggle("gpu_turbo", T_PERF, Store.CONFIG, "NX_GPU_TURBO", "0", OFF_ON, "runtime/src/perf_hint.cpp getenv(NX_GPU_TURBO)").restart());
        l.add(choice("mem_limit", T_PERF, Store.CONFIG, "NX_MEM_LIMIT_GB", null, a(null, "3", "3.5", "4", "5", "6"),
                a("Automatic (60% of RAM)", "3 GB", "3.5 GB", "4 GB", "5 GB", "6 GB"),
                a("Automático (60% da RAM)", "3 GB", "3,5 GB", "4 GB", "5 GB", "6 GB"),
                "runtime/src/memguard_android.cpp getenv(NX_MEM_LIMIT_GB)").restart());
        l.add(choice("affinity", T_PERF, Store.CONFIG, "NX_AFFINITY", null, a(null, "1", "2"),
                a("0 (system decides, default)", "1 (fast cores for the render thread)", "2 (not the small cores)"),
                a("0 (o sistema decide, padrão)", "1 (núcleos rápidos na thread de render)", "2 (sem os núcleos pequenos)"),
                "runtime/src/frame_stats.cpp getenv(NX_AFFINITY)").restart().advanced());
        l.add(choice("chunk_draws", T_PERF, Store.CONFIG, "NX_SUBMIT_CHUNK_DRAWS", null, a(null, "0", "400"),
                a("150 (default)", "0 (one part)", "400"), a("150 (padrão)", "0 (uma parte)", "400"),
                "runtime/src/nvn/nvn_frames.cpp getenv(NX_SUBMIT_CHUNK_DRAWS)").restart().advanced());
        l.add(toggle("pipeline_cache", T_PERF, Store.CONFIG, "NX_PIPELINE_CACHE", null, a("0", null),
                "runtime/src/nvn/nvn_pipeline_cache.cpp getenv(NX_PIPELINE_CACHE)").restart().advanced());
        // NX_PAD_BUFFERS=1: the zero padding after bound buffers again (study intel-igpu-1710 dropped it where the GPU
        // bound-checks); the safe choice if HUD text or skinned characters glitch (DEBUG.md #21).
        l.add(toggle("pad_buffers", T_PERF, Store.CONFIG, "NX_PAD_BUFFERS", null, a(null, "1"),
                "runtime/src/nvn/nvn_sets.cpp getenv(NX_PAD_BUFFERS)").restart().advanced());
        // ---- controls (the runtime draws the buttons: touch_layout.cpp, platform_android_input.cpp)
        l.add(toggle("touch_show", T_CONTROLS, Store.CONFIG, "NX_TOUCH_SHOW", null, a("0", null),
                "runtime/src/platform_android_input.cpp getenv(NX_TOUCH_SHOW)").restart());
        l.add(slider("opacity", 10, 100, 5, 85, "NX_TOUCH_OPACITY", "runtime/src/platform_android_input.cpp getenv(NX_TOUCH_OPACITY)"));
        l.add(slider("size", 50, 160, 5, 100, "NX_TOUCH_SIZE", "runtime/src/touch_layout.cpp getenv(NX_TOUCH_SIZE)"));
        l.add(action("layout", T_CONTROLS));
        l.add(action("mirror", T_CONTROLS));
        l.add(action("resetlayout", T_CONTROLS));
        // ---- audio
        l.add(toggle("mute", T_AUDIO, Store.CONFIG, "NX_MUTE", null, a(null, "1"),
                "runtime/src/platform_android_audio.cpp getenv(NX_MUTE)").restart());
        // ---- data
        l.add(info("state", T_DATA));
        l.add(info("space", T_DATA));
        l.add(action("import", T_DATA));
        l.add(action("saves_export", T_DATA));
        l.add(action("saves_import", T_DATA));
        l.add(action("clearcache", T_DATA));
        l.add(action("wipe", T_DATA));
        // ---- diagnostics
        l.add(choice("fps_mode", T_DIAG, Store.APP, "fps_mode", "0", a("0", "1", "2"), a("Off", "Simple", "Full"),
                a("Desligado", "Simples", "Completo"), "java:FpsOverlay (GameActivity.nativeFrames)"));
        l.add(choice("fps_pos", T_DIAG, Store.APP, "fps_pos", "0", a("0", "1"), a("Top left", "Top right"),
                a("Canto superior esquerdo", "Canto superior direito"), "java:GameActivity.attachOverlay"));
        l.add(toggle("trace", T_DIAG, Store.CONFIG, "trace", "0", OFF_ON, "runtime/src/main.cpp setting(trace)").restart().advanced());
        l.add(action("logpath", T_DIAG));
        l.add(action("diag", T_DIAG));
        l.add(action("device", T_DIAG));
        // ---- about
        l.add(choice("lang", T_ABOUT, Store.APP, "lang", "auto", a("auto", "en", "pt"), a("Automatic", "English", "Portuguese"),
                a("Automático", "English", "Português"), "java:Lang"));
        l.add(toggle("advanced", T_ABOUT, Store.APP, "ui_advanced", "0", OFF_ON, "java:OptionsActivity"));
        l.add(info("version", T_ABOUT));
        l.add(action("credits", T_ABOUT));
        l.add(action("yt", T_ABOUT));
        l.add(action("licenses", T_ABOUT));
        l.add(info("legal", T_ABOUT));
        return l;
    }

    static Prop find(List<Prop> l, String id) {
        for (Prop p : l) if (p.id.equals(id)) return p;
        return null;
    }
}
