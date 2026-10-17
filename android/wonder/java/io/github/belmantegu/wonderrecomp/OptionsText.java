package io.github.belmantegu.wonderrecomp;

import java.util.HashMap;
import java.util.Map;

/** Titles, descriptions and tab names of the options, English and Portuguese (the mockup's wording). */
final class OptionsText {
    private static final Map<String, String[]> T = new HashMap<>();  // id -> {title en, title pt, desc en, desc pt}
    private static final Map<String, String[]> TABS = new HashMap<>();  // tab -> {en, pt, note en, note pt}
    private static final Map<String, String[]> ACT = new HashMap<>();  // action id -> {label en, label pt, icon}

    private static void t(String id, String te, String tp, String de, String dp) {
        T.put(id, new String[] {te, tp, de, dp});
    }

    private static void tab(String id, String e, String p, String ne, String np) {
        TABS.put(id, new String[] {e, p, ne, np});
    }

    private static void act(String id, String e, String p, String icon) {
        ACT.put(id, new String[] {e, p, icon});
    }

    static {
        tab("image", "Image", "Imagem", "Resolution, scaling and filters", "Resolução, ampliação e filtros");
        tab("perf", "Performance", "Desempenho", "Frame submission, GPU and memory", "Envio de quadros, GPU e memória");
        tab("controls", "Controls", "Controles", "On-screen buttons and gamepad", "Botões na tela e controle físico");
        tab("audio", "Sound", "Som", "Sound output", "Saída de som");
        tab("data", "Data", "Dados", "Game files and saves", "Arquivos do jogo e saves");
        tab("diag", "Diagnostics", "Diagnóstico", "FPS, logs and device", "FPS, logs e aparelho");
        tab("about", "About", "Sobre", "Language, credits and licences", "Idioma, créditos e licenças");

        t("handheld", "Console mode", "Modo do console",
                "Handheld: the game renders for 720p, like the Switch in hand (lighter, more fps). TV: renders for 1080p (sharper, heavier).",
                "Portátil: o jogo renderiza para 720p, como o Switch na mão (mais leve, mais fps). TV: renderiza para 1080p (mais nítido, mais pesado).");
        t("render_height", "Internal resolution", "Resolução interna",
                "Size of the rendered picture (16:9, as the game draws it; the phone adds bars on other screens).",
                "Tamanho da imagem renderizada (16:9, como o jogo desenha; o aparelho põe barras em outras telas).");
        t("render_scale", "Window scale", "Escala da janela",
                "Size of the picture relative to the screen; the present pass does the final scaling. Default by GPU (1 on Adreno 7xx+).",
                "Tamanho da imagem em relação à tela; a ampliação final é feita no present pass. Padrão por GPU (1 em Adreno 7xx+).");
        t("fsr", "FSR 1", "FSR 1", "AMD FidelityFX upscaling: renders fewer pixels and rebuilds the picture.",
                "Ampliação AMD FidelityFX: renderiza menos pixels e reconstrói a imagem.");
        t("fsr_sharpness", "FSR sharpness", "Nitidez do FSR", "How much RCAS sharpens the picture after scaling.",
                "Quanto o RCAS afia a imagem depois da ampliação.");
        t("ultrawide", "Widescreen", "Widescreen",
                "Fill the screen: the game draws a wider view of the level to the phone's own shape instead of 16:9 with bars. The HUD stays a centred 16:9 area. A little heavier (more pixels).",
                "Preencher a tela: o jogo desenha uma visão mais larga da fase no formato do celular em vez de 16:9 com faixas. O HUD fica numa área 16:9 centralizada. Um pouco mais pesado (mais pixels).");
        t("display_hz", "Screen rate", "Frequência da tela",
                "The rate the game paces itself on. The game advances one step per frame, so 90/120 Hz make the whole game run faster (music out of sync): an experiment, not a smoother 60.",
                "A frequência pela qual o jogo se guia. O jogo avança um passo por quadro, então 90/120 Hz deixam o jogo inteiro mais rápido (música fora de sincronia): um experimento, não um 60 mais fluido.");
        t("pad_buffers", "Buffer padding", "Preenchimento de buffers",
                "Off (default): faster, the GPU checks the bounds itself. On: the old zero padding, if HUD text or characters glitch.",
                "Desligado (padrão): mais rápido, a GPU confere os limites sozinha. Ligado: o preenchimento antigo, se o texto do HUD ou os personagens falharem.");
        t("fps_cap", "FPS limit", "Limite de FPS",
                "60: the game's own rule (60 fps when the phone keeps up, its own 30 fps mode when heavy). 30: always the 30 fps mode at the right game speed; a steady 33 ms frame, cooler and lighter on battery.",
                "60: a regra do próprio jogo (60 fps quando o aparelho aguenta, o modo de 30 fps dele quando pesado). 30: sempre o modo de 30 fps na velocidade certa; quadro estável de 33 ms, mais frio e gasta menos bateria.");
        t("pipelined", "GPU submits in flight", "Envios de GPU em voo",
                "Pipelined fences: how many submits stay in flight. Automatic pipelines on integrated GPUs (phones): faster (the fountain scene went from 24 to 60 fps on a Galaxy S23). If meshes look exploded in a course (DEBUG-2 #76), choose Synchronous.",
                "Fences em pipeline: quantos envios ficam em voo. Automático usa pipeline em GPUs integradas (celulares): mais rápido (a cena da fonte foi de 24 para 60 fps num Galaxy S23). Se as malhas aparecerem explodidas em alguma fase (DEBUG-2 #76), escolha Síncrono.");
        t("gpu_turbo", "GPU turbo", "GPU turbo", "Turns off the KGSL power control: Adreno at its top clock. Runs hotter.",
                "Desliga o controle de energia KGSL: Adreno no clock máximo. Esquenta mais.");
        t("mem_limit", "Memory limit", "Limite de memória",
                "The guard ends the game above this, instead of the system killing the app.",
                "O guarda encerra o jogo acima disso, em vez de o sistema matar o app.");
        t("affinity", "CPU cores", "Núcleos da CPU", "Where the threads sit on the clusters (big/prime/little).",
                "Onde as threads ficam nos clusters (big/prime/little).");
        t("chunk_draws", "Draws per submitted part", "Draws por parte enviada",
                "Early partial submits inside the same frame.", "Envios parciais antecipados dentro do mesmo quadro.");
        t("pipeline_cache", "Pipeline cache", "Cache de pipelines",
                "VkPipelineCache on disk: fewer stutters after the first run.", "VkPipelineCache em disco: menos engasgos depois da primeira vez.");
        t("touch_show", "On-screen buttons", "Botões na tela",
                "Hides the touch controls and ignores touches (use a controller).", "Esconde os botões virtuais e ignora toques (use um controle).");
        t("opacity", "Button opacity", "Opacidade dos botões", "", "");
        t("size", "Button size", "Tamanho dos botões", "", "");
        t("layout", "Edit buttons", "Editar botões", "Drag each button where you want it.", "Arraste cada botão para onde quiser.");
        t("mirror", "Left-handed mode", "Modo canhoto", "Mirrors the layout; the stick moves to the right half.",
                "Espelha o layout; o analógico passa para a metade direita.");
        t("resetlayout", "Reset layout", "Restaurar layout", "Puts the buttons back where they were.", "Volta os botões à posição original.");
        t("mute", "Mute", "Mudo", "Does not open the audio output; the game keeps time with a timer.",
                "Não abre a saída de áudio; o jogo continua cronometrado por timer.");
        t("state", "Game data", "Dados do jogo", "", "");
        t("space", "Free space", "Espaço livre", "", "");
        t("import", "Import the game", "Importar o jogo", "XCI or NSP + prod.keys, or a data .zip.", "XCI ou NSP + prod.keys, ou um .zip de dados.");
        t("saves_export", "Saves: export", "Saves: exportar", "Writes user/save to a .zip for backup.", "Grava user/save em um .zip para backup.");
        t("saves_import", "Saves: import", "Saves: importar", "Reads a saves .zip (from this or another device).", "Lê um .zip de saves (deste ou de outro aparelho).");
        t("clearcache", "Clear pipeline cache", "Limpar cache de pipelines", "Deletes pipelines_*.bin. Rebuilt on the next run.",
                "Apaga os pipelines_*.bin. Recria na próxima partida.");
        t("wipe", "Delete imported data", "Apagar dados importados", "Removes analysis / extract / spirv. Saves stay.",
                "Remove analysis / extract / spirv. Os saves ficam.");
        t("fps_mode", "FPS counter", "Contador de FPS", "Overlay while playing.", "Sobreposição durante o jogo.");
        t("fps_pos", "Counter position", "Posição do contador", "", "");
        t("trace", "Trace system calls", "Rastrear chamadas de sistema",
                "Logs file system calls and SVCs. Very slow: for diagnosis only.", "Registra file system e SVCs no log. Muito lento: só para diagnóstico.");
        t("logpath", "Logs", "Logs", "nx.log (and the previous run's nx.log.prev).", "nx.log (e o da partida anterior, nx.log.prev).");
        t("diag", "Save diagnostics", "Salvar diagnóstico", "Packs log, device and settings into a file to share.",
                "Empacota log, aparelho e configuração em um arquivo para compartilhar.");
        t("device", "This device", "Este aparelho", "GPU, Vulkan version and what the game needs.", "GPU, versão do Vulkan e o que o jogo exige.");
        t("lang", "App language", "Idioma do app", "", "");
        t("advanced", "Advanced options", "Opções avançadas", "Shows the rows marked ADVANCED.", "Mostra as linhas marcadas como AVANÇADO.");
        t("version", "Version", "Versão", "", "");
        t("credits", "Credits", "Créditos", "Projeto Belmante and everyone who helped.", "Projeto Belmante e quem ajudou.");
        t("yt", "Belmante's channel", "Canal do Belmante", "", "");
        t("licenses", "Licences", "Licenças", "Open-source libraries and assets used.", "Bibliotecas e recursos de código aberto usados.");
        t("legal", "Notice", "Aviso",
                "Non-profit project. It includes no code, assets, keys or firmware of the game. Nintendo and Super Mario are trademarks of Nintendo.",
                "Projeto sem fins lucrativos. Não inclui código, assets, keys ou firmware do jogo. Nintendo e Super Mario são marcas da Nintendo.");

        act("layout", "Edit", "Editar", "pencil");
        act("mirror", "Mirror", "Espelhar", "flip-horizontal-2");
        act("resetlayout", "Reset", "Restaurar", "rotate-ccw");
        act("import", "Import", "Importar", "upload");
        act("saves_export", "Export", "Exportar", "share");
        act("saves_import", "Import", "Importar", "download");
        act("clearcache", "Clear", "Limpar", "eraser");
        act("wipe", "Delete", "Apagar", "trash-2");
        act("logpath", "View", "Ver", "eye");
        act("diag", "Save", "Salvar", "save");
        act("device", "View", "Ver", "eye");
        act("credits", "View", "Ver", "eye");
        act("yt", "Open", "Abrir", "external-link");
        act("licenses", "View", "Ver", "eye");
    }

    private OptionsText() {
    }

    static String title(String id) {
        String[] x = T.get(id);
        return x == null ? id : Lang.s(x[0], x[1]);
    }

    static String desc(String id) {
        String[] x = T.get(id);
        return x == null ? "" : Lang.s(x[2], x[3]);
    }

    static String tabName(String id) {
        return Lang.s(TABS.get(id)[0], TABS.get(id)[1]);
    }

    static String tabNote(String id) {
        return Lang.s(TABS.get(id)[2], TABS.get(id)[3]);
    }

    static String actionLabel(String id) {
        String[] x = ACT.get(id);
        return x == null ? "" : Lang.s(x[0], x[1]);
    }

    static String actionIcon(String id) {
        String[] x = ACT.get(id);
        return x == null ? "chevron-right" : x[2];
    }

    static String tagAdvanced() {
        return Lang.s("ADVANCED", "AVANÇADO");
    }

    static String tagExperimental() {
        return Lang.s("EXPERIMENTAL", "EXPERIMENTAL");
    }

    static String tagRestart() {
        return Lang.s("RESTARTS", "REINICIA");
    }
}
